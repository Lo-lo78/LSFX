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
        assert 'vcpkg_find_acquire_program(PYTHON3)' in rest
        assert 'find_program(LSFX_PYTHON' not in rest
        assert 'ApplyFluidNoteRouting.py' in rest and 'set(SOURCE_PATH "${LSFX_PATCHED_SOURCE_PATH}")' in rest
        assert (overlay / 'ApplyFluidNoteRouting.py').is_file()
        # Execute the generated CMake insertion against a synthetic extraction.
        original = tmp / f'extracted-{version}'; fixture(original, True, True)
        before = snapshot(original)
        cmake = tmp / 'test.cmake'
        cmake.write_text(f'function(vcpkg_find_acquire_program name)\n  if(NOT name STREQUAL \"PYTHON3\")\n    message(FATAL_ERROR \"Unexpected host tool\")\n  endif()\n  set(PYTHON3 \"{Path(sys.executable).as_posix()}\" PARENT_SCOPE)\nendfunction()\nset(SOURCE_PATH "{original.as_posix()}")\nset(CURRENT_PORT_DIR "{overlay.as_posix()}")\n' + rest)
        # Simulate vcpkg's sanitized PATH; only the acquired absolute Python is usable.
        cmake_exe = shutil.which('cmake')
        assert cmake_exe
        subprocess.run([cmake_exe, '-P', str(cmake)], check=True, env={**os.environ, 'PATH': ''})
        assert snapshot(original) == before, 'shared source changed'
        patched = Path(str(original) + '-lsfx-note-routing')
        assert b'lsfx_note_gain' in (patched / 'src/synth/fluid_voice.c').read_bytes()
print('PASS: LF, CRLF, changed context, repeat, missing/duplicate anchor, two supported versions, private CMake source copy with empty PATH and mocked host-tool acquisition, unsupported version rejection')
