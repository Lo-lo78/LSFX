# LSFX 0.99.3 TEST34

28 November 2026 · Windows x64 VST3 · Lo-lo

LSFX is an accessible multitimbral SF2/SFZ instrument for REAPER. Its defining feature is a fully editable MIDI bank/program map combining complete SoundFont banks, individual SF2 presets and SFZ instruments from multiple sources. Save the entire hybrid map as an LSFX preset or export it as a standalone SoundFont. Sixteen independent MIDI channels, 64 stereo audio outputs (Main 1/2 and 63 auxiliary pairs 3/4–127/128), 16 stereo parallel compressors, channel gains, SFZ program-specific gain, shared Main effects and keyboard/screen-reader navigation make it a flexible mixing instrument.

## TEST34 — Instrument Editor (first stage)

Alt+T opens the editor after Export SoundFont and before About. F1 selects the SF2/SFZ format and name, F2 imports mono/stereo WAV/AIFF/FLAC samples, F3 edits mapping zones, F9 saves or exports. Ctrl+Tab / Ctrl+Shift+Tab navigate pages. Escape returns to the main Instrument Editor button. Source audio is held as immutable original bytes in RAM, deduplicated by content and shared by zones and undo snapshots. There is no editor DSP. Ctrl+S / Ctrl+Shift+S save a portable lossless ZIP project (`.lsfx-instrument`) with `project.xml` and internal `Samples/` entries. No external sample paths are required on reopening. Default directory: Documents/LSFX/Instruments. Project limits: 4096 samples and zones, 256 MiB per sample, 1 GiB unique original audio per project. SF2 export is standard PCM16, bank 0/program 0; SFZ export creates a new folder with relative sample paths and only used samples. Existing SF2/SFZ outputs or folders are rejected rather than overwritten. Work in RAM is retained while this plugin instance exists, but not embedded in REAPER project state; explicitly save the instrument project.

F2 import modes: Layer, Chromatic (start note selectable), Multisample (metadata/filename root), Velocity Layers. F3: arrows navigate rows/columns, Page Up/Down moves five rows, Home/End first/last zone, Ctrl+Home/End first/last cell; Alt+Up/Down edit, Alt+Page changes ten steps, Alt+Home max/Alt+End min, Alt+Left/Right step. Insert adds a zone with selected sample; Ctrl+D duplicates; Delete removes zone; Ctrl+C/V copies zone settings preserving target sample; Ctrl+Z/Y undo/redo (in the open editor). F2 only removes unused samples. Level is -60..0 dB for compatible non-destructive SF2 export. Use F9 Export and replace to load a newly exported instrument at the MIDI address shown; the original library file is not changed. Replacement uses the existing import policy (per-program mixer settings at that address are reset). Export-only does not change the map. This first test does not implement live in-memory preview, loop/envelope/filter/choke editing, REAPER item import, MIDI learn or opening existing SF2/SFZ for editing. All heavy I/O occurs on an editor worker, not the audio thread.

## TEST33 — MIDI Reset

Ctrl+Alt+M restores MIDI playback defaults without unloading the sound library or resetting the mixer. Notes are stopped; SF2 channels are reset using FluidSynth's system reset and the virtual programs are rebound (bank 000/program 000, channel 10 bank 128/program 000; standalone SFZ follows the initial open-file policy). SFZ controllers are restored to instrument defaults, pitch wheel and channel pressure reset. Manual CC/generator overrides are cleared. Follow MIDI mode also restores the MIDI-controlled global FX profile; Manual mode preserves global FX settings. Channel gain, output assignments, compressors, SFZ instrument gain, note mixer and map remain intact. The reset is processed before incoming MIDI in the next audio block; it does not reload samples, and is unavailable while empty/loading. This is not a full reset of internal SFZ round-robin/keyswitch state or a guarantee of reproducing an arbitrary mid-song seek; REAPER can still send chased MIDI events after the reset. Ctrl+Enter remains Panic; Alt+I remains full initialization.

## New in 0.99.2

- Selective hybrid SF2/SFZ export to standard PCM16 SoundFont 2.01, with dependency pruning and SFZ conversion warnings.
- Clear a bank or a program with Delete and confirmation, without modifying original sources.
- 64 stereo output destinations; 128 mono output pins including Main.
- SF2 per-note mixer: independent Output, Level, Pan and Pitch per virtual program and note, preserving the original MIDI channel and shared channel compressor.
- Manual MIDI learn in the note grid; direct Alt-key editing and silent navigation boundaries.
- Digits select MIDI channels in Alt+M, Alt+U and Alt+R: 2–9 directly; 1 cycles 1, 10–16, 1.
- Bank/Program navigation in logical columns of eight, matching General MIDI program families; silent boundaries.
- Ctrl+Enter MIDI Panic; Alt+Enter note mixer. Save As, Export SoundFont and About were the last three main-window controls in 0.99.2; TEST34 adds Instrument Editor before About.
- FluidSynth 2.6.2/2.6.3 Windows allocation fix and per-voice routing extension retained.

## User guides

- [English user guide](Doc/LSFX-Guide-EN.html)
- [Guida utente italiana](Doc/LSFX-Guida-IT.html)

In the Windows distribution both HTML guides are included in the Doc folder next to the VST3. No sample banks are bundled. Presets reference external files; keep the sources and SFZ sample trees accessible.

## Essential commands

Alt+O opens a source and replaces the map; Alt+K selects a bank (Enter imports an SF2 bank); Alt+P selects a program (Enter imports SF2/SFZ); Delete clears the selected bank/program after confirmation. Alt+M selects MIDI channel; Alt+L opens parameters; Alt+U channel gains; Alt+R routing; Alt+G master gain; Alt+B preset browser; Ctrl+S save; Ctrl+Shift+S save as; Alt+I initialize with confirmation; Ctrl+Enter MIDI Panic; Alt+Enter note mixer (SF2 per-note audio processing; SFZ assignments stored only); Alt+A About; Alt+S status; Alt+E export SoundFont.

Bank and Program: Up/Down moves one row within the column, Left/Right moves eight positions, Page Up/Down moves up to five rows in the column, Home/End selects column boundaries, Ctrl+Home/End selects whole-grid boundaries. Bank 128 and existing extended banks remain accessible. Program numbers stay 000–127. All boundaries are silent.

## Export SoundFont (Alt+E)

Exports the actual virtual map, including selected individual SF2 programs and banks only, into a standard standalone PCM16 SoundFont 2.01. Shared instruments and samples are reused within each source file; linked stereo samples, generators and modulators are retained. Exported preset names end with a short source filename label (up to eight ASCII characters, disambiguated if needed), or SFZ for converted instruments. The name is shortened as necessary to fit the standard 20-byte SF2 field (19 characters plus terminator). The output uses the virtual destination bank and program numbers, not the original addresses. No LSFX-specific gain, CC overrides, routing, compressors or effects are exported. Source banks are never overwritten. Choose a destination with the Windows Save dialog; Cancel/Escape cancels, including during export. The current map is unchanged. The worker streams PCM to a temporary file before replacing the destination.

SFZ sources are converted into single SF2 presets at their mapped addresses. Supported sampled regions include inherited global/master/group settings, includes and defines, relative sample paths, mono/stereo PCM, key/velocity ranges, pitch, pan, source volume, sample offsets and loops, amplitude envelopes, low-pass filters, and approximate standard SF2 modulation envelopes/LFOs/CC modulators. Sample audio is converted to PCM16; JUCE-supported audio formats are decoded offline. The same SFZ path is converted once per export, and identical sample slices/settings are reused. The SFZ conversion is approximate: unsupported opcodes, omitted regions, and clipping are reported. Round robin uses first position, random selection the midpoint, controller gates their initial values, keyswitches their declared default articulation. Release-triggered regions are omitted; one-shot is approximated by a long release. Oscillators without audio samples and missing/undecodable samples fail the export. No changes are made to source files or the map. Space or Enter dismisses the completion dialog.

SF3, 24-bit SF2, nonportable banks above 128, ROM samples or selected SF2 sample offsets outside nominal sample bounds remain unsupported. Invalid unused loop coordinates in old files are repaired. Deduplication is by source identity, not across unrelated source files. Original licenses continue to apply. Do not redistribute exports unless permitted by all source licenses.

## Note mixer and compatibility

Alt+Enter opens the note grid with MIDI learn off. Alt+M toggles learning; plain arrows navigate, Alt+arrows edit. The four columns are Output, Level, Pan and Pitch. Assignments are per virtual bank/program, affect newly started SF2 voices and are saved in LSFX presets and host state, not exported to SF2. A note with Channel Default follows the channel's route. SFZ per-note processing is not yet implemented; SFZ channel-level audio routing is available. The 128 internal SF2 mixer groups include 112 additional channel/destination combinations; Alt+S reports exhaustion. Special legato modes that reuse a voice are not verified. EQ is not included in this release.

Older preset route indices are preserved. Previously recorded host automation for routing may need to be recreated due to the expanded normalized choice range. Older LSFX versions cannot interpret map deletion markers.

## Build and verification

Push to main and download the Windows x64 artifact from GitHub Actions. The default build retains mandatory native multigroup and hybrid processor tests. The user validated TEST31.3 per-note SF2 routing and TEST32 keyboard ergonomics in REAPER. TEST33 MIDI Reset was validated by the user. TEST34 adds the Instrument Editor; full Windows compilation and host verification of TEST34 are required. The optional SF2 stereo recovery build is diagnostic only.

Plug-in IDs and existing state format are unchanged. See [testing](TESTING.md), [third-party and licensing notes](THIRD_PARTY.md) and [development history](Doc/DEVELOPMENT_HISTORY.md). Complete dependency distribution obligations before publishing binaries.
