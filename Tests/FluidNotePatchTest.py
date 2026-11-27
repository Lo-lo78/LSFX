"""Exercise the patch installer without downloading or compiling FluidSynth."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import shutil
import json
import os
sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parents[1] / 'Tools'
sys.path.insert(0, str(TOOLS))
from ApplyFluidNoteRouting import apply, changes, PATCH


def fixture(root, crlf=False, changed_context=False):
    # Reconstruct the unmodified hunk contexts with unrelated lines between.
    for section in PATCH.read_text().split('diff --git ')[1:]:
        path = root / section.splitlines()[0].split()[0][2:]
        path.parent.mkdir(parents=True, exist_ok=True)
        blocks = []
        for hunk in re.split(r'^@@ .*? @@[^\n]*\n', section, flags=re.M)[1:]:
            blocks.append('\n'.join(x[1:] for x in hunk.splitlines() if x.startswith((' ', '-'))))
        text = '\n// unrelated source between hunks\n'.join(blocks) + '\n'
        if changed_context:
            text = text.replace('/* Set up buffer mapping, should be done more flexible in the future. */',
                                '/* Revised unrelated comment. */\n')
            text = text.replace('    i = 2 * (voice->chan % channel->synth->audio_groups);',
                                '\t i = 2 * (voice->chan % channel->synth->audio_groups);  ')
        path.write_bytes(text.replace('\n', '\r\n' if crlf else '\n').encode())


def snapshot(root):
    return {p.relative_to(root): p.read_bytes() for p in root.rglob('*') if p.is_file()}


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for crlf, changed in [(False, False), (True, False), (True, True)]:
        source = tmp / f'{crlf}-{changed}'
        fixture(source, crlf, changed)
        apply(source)
        after = snapshot(source)
        apply(source)
        assert snapshot(source) == after, 'not idempotent'
        assert b'lsfx_note_gain' in after[Path('src/synth/fluid_voice.c')]
        assert b'lsfx_fluid_noteon' in after[Path('src/synth/fluid_synth.c')]
    for duplicate in (False, True):
        source = tmp / f'bad-{duplicate}'
        fixture(source)
        path = source / 'src/synth/fluid_voice.c'
        text = path.read_text()
        anchor = '        voice->pan = fluid_voice_gen_value(voice, GEN_PAN);'
        path.write_text(text + anchor + '\n' if duplicate else text.replace(anchor, '        unsupported change;'))
        before = snapshot(source)
        try:
            apply(source)
        except ValueError:
            pass
        else:
            raise AssertionError('missing or ambiguous code anchor accepted')
        assert snapshot(source) == before, 'failed patch modified files'
    # Validate that the overlay injects only the VLA into vcpkg PATCHES,
    # and applies the note-routing delta afterwards, from its private copy.
    port = tmp / 'vcpkg/ports/fluidsynth'; port.mkdir(parents=True)
    (port / 'portfile.cmake').write_text('vcpkg_from_github(\n    OUT_SOURCE_PATH SOURCE_PATH\n    PATCHES\n        vendor.patch\n)\n# following steps\n')
    for version in ('2.6.2', '2.6.3', '2.6.4'):
        (port / 'vcpkg.json').write_text(json.dumps({'version': version}))
        result = subprocess.run([sys.executable, str(TOOLS / 'PrepareFluidSynthOverlay.py'), str(tmp / 'vcpkg'), str(tmp / 'overlay')], capture_output=True, text=True)
        if version == '2.6.4':
            assert result.returncode != 0
            continue
        assert result.returncode == 0, result.stderr
        overlay = tmp / 'overlay/fluidsynth'
        script = (overlay / 'portfile.cmake').read_text()
        extraction, rest = script.split('\n)\n', 1)
        assert 'fluidsynth-vla-size.patch' in extraction and 'vendor.patch' in extraction
        assert 'fluidsynth-note-routing.patch' not in extraction
        assert 'execute_process' not in rest and 'vcpkg_find_acquire_program' not in rest
        assert 'ApplyFluidNoteRouting.cmake' in rest and 'set(SOURCE_PATH "${LSFX_PATCHED_SOURCE_PATH}")' in rest
        assert (overlay / 'ApplyFluidNoteRouting.cmake').is_file()
        cmake_exe = shutil.which('cmake')
        assert cmake_exe
        def run_cmake(code, success=True):
            cmake = tmp / 'test.cmake'
            cmake.write_text('cmake_minimum_required(VERSION 3.22)\n' + code, encoding='utf-8')
            result = subprocess.run([cmake_exe, '-P', str(cmake)], capture_output=True, text=True, env={**os.environ, 'PATH': ''})
            if success:
                assert result.returncode == 0, result.stdout + result.stderr
            else:
                assert result.returncode != 0 and 'patch refused' in result.stderr, result.stdout + result.stderr
            return result
        def canonical(root):
            return {p: b.replace(b'\r\n', b'\n') for p,b in snapshot(root).items()}
        # Compare actual CMake output with the already reviewed Python reference.
        for crlf, changed in [(False, False), (True, False), (True, True)]:
            original = tmp / f'extracted {version}-{crlf}-{changed}'
            fixture(original, crlf, changed)
            before = snapshot(original)
            reference = tmp / 'reference'
            shutil.copytree(original, reference, dirs_exist_ok=True)
            apply(reference)
            code = f'set(SOURCE_PATH "{original.as_posix()}")\nset(CURRENT_PORT_DIR "{overlay.as_posix()}")\n' + rest
            run_cmake(code)
            assert snapshot(original) == before, 'shared source changed'
            patched = Path(str(original) + '-lsfx-note-routing')
            assert canonical(patched) == canonical(reference), 'native CMake differs from Python reference'
            once = snapshot(patched)
            direct = f'include("{(overlay / "ApplyFluidNoteRouting.cmake").as_posix()}")\nlsfx_apply_note_routing("{patched.as_posix()}")\n'
            run_cmake(direct)
            assert snapshot(patched) == once, 'native CMake is not idempotent'
        for duplicate in (False, True):
            broken = tmp / f'native-bad-{version}-{duplicate}'
            fixture(broken)
            path = broken / 'src/synth/fluid_voice.c'
            text = path.read_text()
            anchor = '        voice->pan = fluid_voice_gen_value(voice, GEN_PAN);'
            path.write_text(text + anchor + '\n' if duplicate else text.replace(anchor, '        unsupported change;'))
            before = snapshot(broken)
            run_cmake(f'include("{(overlay / "ApplyFluidNoteRouting.cmake").as_posix()}")\nlsfx_apply_note_routing("{broken.as_posix()}")\n', success=False)
            assert snapshot(broken) == before, 'failed native patch modified files'
print('PASS: native CMake with empty PATH, LF/CRLF, changed context, paths with spaces, Python reference equality, repeat, missing/duplicate anchor, two supported versions, unmodified shared extraction, unsupported version rejection')
