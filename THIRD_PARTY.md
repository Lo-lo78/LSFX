# Third-party components and release checklist

- JUCE 8.0.12: https://github.com/juce-framework/JUCE/tree/8.0.12 ; license: https://github.com/juce-framework/JUCE/blob/8.0.12/LICENSE.md (AGPLv3 or commercial JUCE licence).
- FluidSynth: https://github.com/FluidSynth/fluidsynth ; LGPL-2.1-or-later. CMake uses FluidSynth::libfluidsynth from vcpkg. SF3 is enabled using the sndfile feature, including its codec dependencies.
- vcpkg: https://github.com/microsoft/vcpkg ; the workflow records the installed dependency versions and copies available copyright notices.
- No proprietary BASS or BASSMIDI library is used.
- No SoundFont is included. SoundFont authors' licenses are independent of the synthesis engine.

TEST1 uses static linkage with the dynamic Microsoft CRT (x64-windows-static-md), avoiding a separate FluidSynth DLL at runtime. Before a public binary release, confirm an appropriate project licence and supply all required license texts and corresponding source, including the exact dependency sources, patches/build information and a compliant means of relinking modified LGPL libraries as applicable. The CI package includes notices and dependency identifiers, but is not a substitute for this release-compliance work. If preferred, we can switch to a separately packaged shared FluidSynth library and verify runtime DLL loading. For non-AGPL distribution a commercial JUCE licence may be needed.

TEST2 adds sfizz 1.2.3 (BSD-2-Clause), https://github.com/sfztools/sfizz/tree/1.2.3, plus its bundled dependencies. Review their licenses and include required notices before public distribution. The workflow includes the sfizz top-level license; this alone is not a complete compliance review.

TEST3 guards duplicate cxxopts alias declarations in the pinned sfizz dependency. TEST4 additionally patches SfizzConfig.cmake to use the dynamic Microsoft CRT and normalizes the runtime property across sfizz and bundled targets. Static library linkage is retained.

TEST5 fixes the sfizz 1.2.3 license path to LICENSE and records packaging logs.

TEST9 reuses the incremental parameter search and search-key mapping from the user's LJuno-116 0.99.67 source, licensed AGPL-3.0-or-later. Preserve attribution and the applicable licence terms. The licence notice from that source is included in Doc/LJuno-116-LICENSE.md.

TEST19 also copies the LJuno-116 PresetBrowserMemory.h helper (AGPL-3.0-or-later) and adapts its folder browser workflow. See Doc/LJuno-116-LICENSE.md.

TEST25 adapts the LJuno-116 0.99.67 stereo compressor detector and control ranges (see Doc/LJuno-116-LICENSE.md), with independent channel states and a zero-mix default.

## FluidSynth 2.6.2 Windows patch (TEST26.1)

Upstream source: https://github.com/FluidSynth/fluidsynth/tree/v2.6.2 (LGPL-2.1-or-later). LSFX applies `Tools/fluidsynth-vla-size.patch` using a vcpkg overlay; the patch is included in the Windows distribution. It parenthesizes the length expression of the non-VLA fallback in `src/utils/fluidsynth_priv.h`, preventing an undersized stack allocation for compound expressions. No other dependency source changes are introduced by this patch.

## 0.99.1

About panel and caret navigation adapted from LJuno-116 0.99.67 (AGPL-3.0-or-later). The public release candidate keeps the existing static linkage and vcpkg overlay. Complete the distribution-compliance checklist above before distributing binaries; the provided notices alone do not establish compliance.

## TEST27.1: FluidSynth 2.6.3

The runner now supplies FluidSynth 2.6.3. The upstream header in [v2.6.3](https://github.com/FluidSynth/fluidsynth/blob/v2.6.3/src/utils/fluidsynth_priv.h) still contains the same non-VLA allocation expression. The existing patch is retained for both 2.6.2 and 2.6.3; unknown versions are rejected. The mandatory native multigroup and hybrid processor tests remain enabled.
