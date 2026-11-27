"""Copy the vcpkg port and apply the reviewed VLA and note routing fixes.

Fail explicitly if the port has changed rather than silently building an
unpatched dependency. The patch is part of the vcpkg ABI/cache key.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
from GenerateFluidNoteRoutingCmake import generate

parser = argparse.ArgumentParser()
parser.add_argument('vcpkg_root', type=Path)
parser.add_argument('overlay_root', type=Path)
args = parser.parse_args()
source = args.vcpkg_root / 'ports' / 'fluidsynth'
manifest = json.loads((source / 'vcpkg.json').read_text(encoding='utf-8-sig'))
version = next((manifest[key] for key in ('version', 'version-semver', 'version-string', 'version-date') if key in manifest), None)
# Both upstream tags contain the same unsafe allocation macro.
# Keep a strict allowlist and let vcpkg fail if the patch no longer applies.
if version not in ('2.6.2', '2.6.3'):
    raise SystemExit(f'Expected FluidSynth 2.6.2 or 2.6.3 port, found {version}; review VLA fix before upgrading.')
target = args.overlay_root / 'fluidsynth'
if target.exists():
    shutil.rmtree(target)
shutil.copytree(source, target)
# The VLA patch is still applied by vcpkg. The note-routing patch is applied
# using unique C code anchors after source extraction, not unified diff context.
for name in ('fluidsynth-vla-size.patch', 'fluidsynth-note-routing.patch', 'ApplyFluidNoteRouting.py'):
    item = Path(__file__).with_name(name)
    (target / name).write_bytes(item.read_text(encoding='utf-8-sig').encode('utf-8'))
(target / "ApplyFluidNoteRouting.cmake").write_text(generate(), encoding="utf-8")
patches = [target / 'fluidsynth-vla-size.patch']
portfile = target / 'portfile.cmake'
text = portfile.read_text(encoding='utf-8-sig')
text, count = re.subn(r'(?m)^([ \t]*)PATCHES[ \t]*$', lambda m: m.group(0) + ''.join('\n' + m.group(1) + '    ' + patch.name for patch in patches), text, count=1)
if count != 1:
    raise SystemExit('FluidSynth portfile has no PATCHES block; refusing unpatched build.')
# The port's first vcpkg_from_github call sets SOURCE_PATH. Work on a private
# copy so the normal vcpkg source cache remains unmodified.
match = re.search(r'(?ms)^vcpkg_from_github\(.*?^\)[ \t]*$', text)
if not match or not re.search(r'\bOUT_SOURCE_PATH[ \t]+SOURCE_PATH\b', match.group(0)):
    raise SystemExit('FluidSynth port extraction has changed; refusing unpatched build.')
extra = r'''
# LSFX TEST31.3: apply the same reviewed delta by code anchors.
set(LSFX_PATCHED_SOURCE_PATH "${SOURCE_PATH}-lsfx-note-routing")
file(REMOVE_RECURSE "${LSFX_PATCHED_SOURCE_PATH}")
file(MAKE_DIRECTORY "${LSFX_PATCHED_SOURCE_PATH}")
file(COPY "${SOURCE_PATH}/" DESTINATION "${LSFX_PATCHED_SOURCE_PATH}")
# Apply directly in the current CMake process, without launching Python.
include("${CURRENT_PORT_DIR}/ApplyFluidNoteRouting.cmake")
lsfx_apply_note_routing("${LSFX_PATCHED_SOURCE_PATH}")
set(SOURCE_PATH "${LSFX_PATCHED_SOURCE_PATH}")
'''
text = text[:match.end()] + '\n' + extra + text[match.end():]
portfile.write_text(text, encoding='utf-8')
print(f'FluidSynth {version}: prepared patched overlay at {target}')
