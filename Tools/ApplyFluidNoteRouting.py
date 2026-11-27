"""Apply the LSFX voice-routing delta by unique code anchors.

The original unified diff is the source of truth. vcpkg/git apply can reject
irrelevant context (including blank lines); only exact changed code and the
preceding context for insertions are required here. Never use fuzzy matching.
All files are validated before any file is written. Safe to run twice.
"""
from pathlib import Path
import argparse
import re

PATCH = Path(__file__).with_name('fluidsynth-note-routing.patch')
EXPECTED_FILES = {'src/synth/fluid_synth.h', 'src/synth/fluid_voice.h',
                  'src/synth/fluid_voice.c', 'src/synth/fluid_synth.c'}


def changes(patch):
    result = {}
    for section in patch.split('diff --git ')[1:]:
        path = section.splitlines()[0].split()[0][2:]
        if path not in EXPECTED_FILES or path in result:
            raise ValueError(f'Unexpected patch path: {path}')
        edits = []
        for hunk in re.split(r'^@@ .*? @@[^\n]*\n', section, flags=re.M)[1:]:
            lines = hunk.splitlines()
            before = []
            i = 0
            while i < len(lines):
                line = lines[i]
                if line.startswith(' '):
                    before.append(line[1:]); i += 1
                    continue
                if not line.startswith(('+', '-')):
                    raise ValueError(f'Unexpected diff line: {line!r}')
                old, new = [], []
                while i < len(lines) and lines[i].startswith(('+', '-')):
                    line = lines[i]
                    (old if line[0] == '-' else new).append(line[1:])
                    i += 1
                if not old:
                    # Pure insertion: anchor to the immediately preceding
                    # nonempty context, plus any intervening blank lines.
                    if not before or not any(x.strip() for x in before):
                        raise ValueError('Unanchored insertion')
                    old = before[-min(2, len(before)):]
                    if not any(x.strip() for x in old):
                        raise ValueError('Blank insertion anchor')
                    new = old + new
                edits.append((old, new))
                before = []
        result[path] = edits
    if set(result) != EXPECTED_FILES or sum(map(len, result.values())) != 10:
        raise ValueError('Unexpected patch contents; review before compiling')
    return result


def normalized(line):
    # Only horizontal whitespace at line boundaries; never ignore tokens.
    return line.strip(' \t')


def positions(lines, block):
    wanted = list(map(normalized, block))
    current = list(map(normalized, lines))
    return [i for i in range(len(current) - len(wanted) + 1)
            if current[i:i+len(wanted)] == wanted]


def apply(root, patch=PATCH):
    pending = {}
    for relative, edits in changes(patch.read_text(encoding='utf-8-sig')).items():
        path = root / relative
        raw = path.read_bytes()
        lines = raw.decode('utf-8').splitlines()
        for old, new in edits:
            original = positions(lines, old)
            applied = positions(lines, new)
            if len(applied) == 1 and (not original or (old == new[:len(old)] and original == applied)):
                continue
            if len(original) != 1 or applied:
                raise ValueError(f'{relative}: expected one unmodified anchor, found {len(original)} old and {len(applied)} new: {old[0].strip()}')
            i = original[0]
            # Keep source formatting for unchanged insertion anchor lines.
            if old == new[:len(old)]:
                new = lines[i:i+len(old)] + new[len(old):]
            lines[i:i+len(old)] = new
        # Preserve native EOL, avoid writing unchanged files on repeated run.
        eol = '\r\n' if b'\r\n' in raw else '\n'
        data = (eol.join(lines) + eol).encode('utf-8')
        pending[path] = data
    for path, data in pending.items():
        if path.read_bytes() != data:
            path.write_bytes(data)
    print('LSFX FluidSynth note routing: 10 unique code anchors verified in 4 source files')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source_root', type=Path)
    args = parser.parse_args()
    try:
        apply(args.source_root)
    except (OSError, ValueError) as error:
        raise SystemExit(f'LSFX note routing patch refused: {error}')
