# LSFX 0.99.2 TEST30 — 64 stereo outputs

27 November 2026 · Windows x64 VST3 · Lo-lo

LSFX is an accessible multitimbral SF2/SFZ instrument for REAPER. Its defining feature is a fully editable MIDI bank/program map combining complete SoundFont banks, individual SF2 presets and SFZ instruments from multiple sources. Save the entire hybrid map as an LSFX preset. Sixteen independent MIDI channels, 64 stereo audio outputs (Main 1/2 and 63 auxiliary pairs 3/4–127/128), 16 stereo parallel compressors, channel gains, SFZ program-specific gain, shared Main effects and keyboard/screen-reader navigation make it a flexible mixing instrument.

## User guides

- [English user guide](Doc/LSFX-Guide-EN.html)
- [Guida utente italiana](Doc/LSFX-Guida-IT.html)

In the Windows distribution both HTML guides are included in the Doc folder next to the VST3. No sample banks are bundled. Presets reference external files; keep the sources and SFZ sample trees accessible.

## Essential commands

Alt+O opens a source and replaces the map; Alt+K selects a bank (Enter imports an SF2 bank); Alt+P selects a program (Enter imports SF2/SFZ); Alt+M selects MIDI channel; Alt+L opens parameters; Alt+U channel gains; Alt+R routing; Alt+G master gain; Alt+B preset browser; Ctrl+S save; Ctrl+Shift+S save as; Alt+I initialize with confirmation; Ctrl+Enter MIDI Panic; Alt+Enter note mixer editor draft (not applied to audio); Alt+A About; Alt+S status; Alt+E export SoundFont.

## Build and verification

Push to main and download the Windows x64 artifact from GitHub Actions. The default build retains the FluidSynth VLA allocation correction from TEST26.1 and the mandatory native multigroup and hybrid processor tests. The user confirmed TEST26.1 worked in REAPER on Windows. The 0.99.1 About and shortcut changes still require compilation and screen-reader verification. The optional SF2 stereo recovery build is diagnostic only.

The plug-in IDs and existing state format are unchanged. See [testing](TESTING.md), [third-party and licensing notes](THIRD_PARTY.md) and [development history](Doc/DEVELOPMENT_HISTORY.md).

## TEST28: Export SoundFont (Alt+E)

Exports the actual virtual map, including selected individual SF2 programs and banks only, into a standard standalone PCM16 SoundFont 2.01. Shared instruments and samples are reused within each source file; linked stereo samples, generators, modulators and source preset names are retained. The output uses the virtual destination bank and program numbers, not the original addresses. No LSFX-specific gain, CC overrides, routing, compressors or effects are exported. Source banks are never overwritten. Choose a destination with the Windows Save dialog; Cancel/Escape cancels, including during export. The current map is unchanged. The worker streams PCM to a temporary file before replacing the destination.

SFZ sources are converted into single SF2 presets at their mapped addresses. Supported sampled regions include inherited global/master/group settings, includes and defines, relative sample paths, mono/stereo PCM, key/velocity ranges, pitch, pan, source volume, sample offsets and loops, amplitude envelopes, low-pass filters, and approximate standard SF2 modulation envelopes/LFOs/CC modulators. Sample audio is converted to PCM16; JUCE-supported audio formats are decoded offline. The same SFZ path is converted once per export, and identical sample slices/settings are reused. The SFZ conversion is approximate: unsupported opcodes, omitted regions, and clipping are reported. Round robin uses first position, random selection the midpoint, controller gates their initial values, keyswitches their declared default articulation. Release-triggered regions are omitted; one-shot is approximated by a long release. Oscillators without audio samples and missing/undecodable samples fail the export. No changes are made to source files or the map. Space or Enter dismisses the completion dialog.

SF3, 24-bit SF2, nonportable banks above 128, ROM samples or selected SF2 sample offsets outside nominal sample bounds remain unsupported. Invalid unused loop coordinates in old files are repaired. Deduplication is by source identity, not across unrelated source files. Original licenses continue to apply. Do not redistribute exports unless permitted by all source licenses.

TEST27.1 build fix: accept the reviewed FluidSynth 2.6.3 vcpkg port while retaining the Windows VLA patch and all native smoke tests.

## TEST29: map deletion and note editor draft

- Delete on Bank or Program requests confirmation, with Cancel initially focused. Clear removes only the chosen destination bank or program from the virtual map; source files are never deleted. The deletion is saved with the LSFX preset/host state and honored by SoundFont export. Enter still imports. Reimporting a deleted address fills it again. Clearing the last mapped program unloads the engine without resetting the mixer. Deleting a location clears its instrument gain and draft note assignments; other locations are unchanged. Older LSFX builds cannot interpret deletion markers, so retain a copy of important presets before opening them in older versions.
- Ctrl+Enter is MIDI Panic; Alt+Enter opens the note mixer draft for the selected MIDI channel's mapped program. Alt+M still selects MIDI channel outside the draft; inside it toggles MIDI learn. Playing a note on that channel selects the note and enters Output editing. Other MIDI channels and Note Off do not select a note. Enter toggles edit/navigation. Left/right moves between Output, Level, Pan and Pitch. In navigation, Up/Down selects rows; Alt+Up/Down edits the cell; in edit mode Up/Down edits. Home/End moves to column boundaries, Ctrl+Home/End to whole-grid boundaries. Backspace resets the cell; Delete removes the note assignment. Esc or Alt+Enter closes and returns to the previous focus. Only modified notes and the temporary selected learn note appear. Settings are stored per bank/program and saved in LSFX state, not exported to SF2.
- **Important: note mixer is a UI/state/MIDI-learn draft, explicitly labeled as such. Its Output, Level, Pan and Pitch values do not change the audio yet.** Per-voice engine routing, shared compression across split outputs, MIDI sustain, exclusive classes and SFZ articulation behavior still need to be implemented and validated. Existing audio rendering and export behavior remain unchanged.

## TEST30: expanded host output routing

- 64 stereo buses (128 mono pins) including Main 1/2. All sixteen MIDI channels may select any destination; existing integer route indices retain their meaning. The FluidSynth internal dry groups and channel compressors remain at 16. The note mixer draft accepts all new destinations but is still not applied to audio.
- Old preset/project route indices are preserved. Previously recorded host automation for routing may need to be recreated because the normalized choice range has changed.
