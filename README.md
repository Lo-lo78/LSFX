# LSFX 0.99.2 TEST27.1 — selective SF2 export

27 November 2026 · Windows x64 VST3 · Lo-lo

LSFX is an accessible multitimbral SF2/SFZ instrument for REAPER. Its defining feature is a fully editable MIDI bank/program map combining complete SoundFont banks, individual SF2 presets and SFZ instruments from multiple sources. Save the entire hybrid map as an LSFX preset. Sixteen independent MIDI channels, 17 stereo audio outputs (Main 1/2 and 3/4–33/34), 16 stereo parallel compressors, channel gains, SFZ program-specific gain, shared Main effects and keyboard/screen-reader navigation make it a flexible mixing instrument.

## User guides

- [English user guide](Doc/LSFX-Guide-EN.html)
- [Guida utente italiana](Doc/LSFX-Guida-IT.html)

In the Windows distribution both HTML guides are included in the Doc folder next to the VST3. No sample banks are bundled. Presets reference external files; keep the sources and SFZ sample trees accessible.

## Essential commands

Alt+O opens a source and replaces the map; Alt+K selects a bank (Enter imports an SF2 bank); Alt+P selects a program (Enter imports SF2/SFZ); Alt+M selects MIDI channel; Alt+L opens parameters; Alt+U channel gains; Alt+R routing; Alt+G master gain; Alt+B preset browser; Ctrl+S save; Ctrl+Shift+S save as; Alt+I initialize with confirmation; Alt+Enter MIDI Panic; Alt+A About; Alt+S status; Alt+E export SoundFont.

## Build and verification

Push to main and download the Windows x64 artifact from GitHub Actions. The default build retains the FluidSynth VLA allocation correction from TEST26.1 and the mandatory native multigroup and hybrid processor tests. The user confirmed TEST26.1 worked in REAPER on Windows. The 0.99.1 About and shortcut changes still require compilation and screen-reader verification. The optional SF2 stereo recovery build is diagnostic only.

The plug-in IDs and existing state format are unchanged. See [testing](TESTING.md), [third-party and licensing notes](THIRD_PARTY.md) and [development history](Doc/DEVELOPMENT_HISTORY.md).

## TEST27.1: Export SoundFont (Alt+E)

Exports the actual virtual map, including selected individual SF2 programs and banks only, into a standard standalone PCM16 SoundFont 2.01. Shared instruments and samples are reused within each source file; linked stereo samples, generators, modulators and source preset names are retained. The output uses the virtual destination bank and program numbers, not the original addresses. No LSFX-specific gain, CC overrides, routing, compressors or effects are exported. Source banks are never overwritten. Choose a destination with the Windows Save dialog; Cancel/Escape cancels, including during export. The current map is unchanged. The worker streams PCM to a temporary file before replacing the destination.

This test is limited to PCM16 SF2. Maps containing SFZ, SF3, 24-bit SF2, nonportable banks above 128, ROM samples or selected sample offsets outside nominal sample bounds are rejected rather than silently converted or partially exported. Invalid unused loop coordinates in old files are repaired. Deduplication is by source identity, not across unrelated source files. Original licenses continue to apply. Do not redistribute exports unless permitted by all source licenses. SFZ conversion is a separate development stage.

TEST27.1 build fix: accept the reviewed FluidSynth 2.6.3 vcpkg port while retaining the Windows VLA patch and all native smoke tests.
