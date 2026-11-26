"""Copy the runner's vcpkg port and add the FluidSynth 2.6.2 MSVC VLA fix.

Fail explicitly if the port has changed rather than silently building an
unpatched dependency. The patch is part of the vcpkg ABI/cache key.
"""
import argparse
import json
from pathlib import Path
import re
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('vcpkg_root', type=Path)
parser.add_argument('overlay_root', type=Path)
args = parser.parse_args()
source = args.vcpkg_root / 'ports' / 'fluidsynth'
manifest = json.loads((source / 'vcpkg.json').read_text(encoding='utf-8-sig'))
version = next((manifest[key] for key in ('version', 'version-semver', 'version-string', 'version-date') if key in manifest), None)
if version != '2.6.2':
    raise SystemExit(f'Expected FluidSynth 2.6.2 port, found {version}; review VLA fix before upgrading.')
target = args.overlay_root / 'fluidsynth'
if target.exists():
    shutil.rmtree(target)
shutil.copytree(source, target)
patch = Path(__file__).with_name('fluidsynth-vla-size.patch')
shutil.copy2(patch, target / patch.name)
portfile = target / 'portfile.cmake'
text = portfile.read_text(encoding='utf-8-sig')
text, count = re.subn(r'(?m)^([ \t]*)PATCHES[ \t]*$', lambda m: m.group(0) + '\n' + m.group(1) + '    ' + patch.name, text, count=1)
if count != 1:
    raise SystemExit('FluidSynth portfile has no PATCHES block; refusing unpatched build.')
portfile.write_text(text, encoding='utf-8')
print(f'FluidSynth {version}: prepared patched overlay at {target}')
