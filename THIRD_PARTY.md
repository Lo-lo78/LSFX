# Third-party components and release checklist

- JUCE 8.0.12: https://github.com/juce-framework/JUCE/tree/8.0.12 ; license: https://github.com/juce-framework/JUCE/blob/8.0.12/LICENSE.md (AGPLv3 or commercial JUCE licence).
- FluidSynth: https://github.com/FluidSynth/fluidsynth ; LGPL-2.1-or-later. CMake uses FluidSynth::libfluidsynth from vcpkg. SF3 is enabled using the sndfile feature, including its codec dependencies.
- vcpkg: https://github.com/microsoft/vcpkg ; the workflow records the installed dependency versions and copies available copyright notices.
- No proprietary BASS or BASSMIDI library is used.
- No SoundFont is included. SoundFont authors' licenses are independent of the synthesis engine.

TEST1 uses static linkage with the dynamic Microsoft CRT (x64-windows-static-md), avoiding a separate FluidSynth DLL at runtime. Before a public binary release, confirm an appropriate project licence and supply all required license texts and corresponding source, including the exact dependency sources, patches/build information and a compliant means of relinking modified LGPL libraries as applicable. The CI package includes notices and dependency identifiers, but is not a substitute for this release-compliance work. If preferred, we can switch to a separately packaged shared FluidSynth library and verify runtime DLL loading. For non-AGPL distribution a commercial JUCE licence may be needed.

TEST2 adds sfizz 1.2.3 (BSD-2-Clause), https://github.com/sfztools/sfizz/tree/1.2.3, plus its bundled dependencies. Review their licenses and include required notices before public distribution. The workflow includes the sfizz top-level license; this alone is not a complete compliance review.

TEST3 applies cmake/PatchSfizz.cmake to the pinned sfizz dependency to guard duplicate cxxopts alias declarations.
