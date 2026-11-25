# LSoundFont 0.0.1 TEST1

Experimental accessible SoundFont VST3 instrument for Windows x64. Source-only initial commit; not a stable release.

## Features in this first build

- Direct SF2 file loading through FluidSynth; no Lua, JSFX, extraction or conversion.
- SF3 decoder enabled by the CI dependency configuration (not tested yet).
- One SoundFont per instance, 16 independent MIDI channels, channel 10 defaults to bank 128 (drums), 256 sample voices.
- Actual bank/program names from the SoundFont, with keyboard-operable JUCE controls.
- MIDI notes, bank select (GS convention), program change, CC, sustain, pitch bend, channel and polyphonic pressure, supported SysEx.
- Seventh-order interpolation, FluidSynth reverb and chorus enabled with engine defaults.
- Stereo output, automatable output gain, panic button.
- Background SoundFont loading; old engine remains active until the new one is ready, then switches. Failed replacement clears the old engine to avoid accidentally playing the wrong bank. No SoundFont is loaded at startup unless a saved state requests it.
- REAPER project and host preset state: full file path, selected bank/program per channel, output gain. Audio samples are not embedded; moving the file requires opening it again. MIDI controller state and in-flight voices are not serialized.

## GitHub build

Create an empty repository (suggested name LSoundFont), extract the contents of this directory into the Git repository root (including the hidden .github directory), commit and push to main. The workflow Build LSoundFont VST3 Windows x64 installs FluidSynth using vcpkg and downloads JUCE 8.0.12. In Actions download the artifact LSoundFont-0.0.1-TEST1-Windows-x64. The artifact contains the installable VST3 ZIP. No automatic release publishing. The CI runner's vcpkg revision is recorded in the package; dependency baseline is not pinned in TEST1.

## First test in REAPER

1. Extract the compiled ZIP. Copy the entire LSoundFont.vst3 bundle into C:\\Program Files\\Common Files\\VST3. Do not copy just the internal binary.
2. Rescan plugins. Insert VST3i: LSoundFont on a track.
3. Open SoundFont, select your original FluidR3 GM.SF2 anywhere on disk. Wait for the status to say Loaded. Read status opens a message box containing the full path/status if necessary.
4. Tab order: Open SoundFont, MIDI channel, Bank, Program, Output gain, Panic, Read status. Use the normal keyboard controls for combo boxes and sliders. NVDA behavior and host focus still require testing.
5. Default UI channel is 1, bank 0/program 0. Change bank/program with combo boxes. MIDI from every channel is independently processed; no Omni remapping in TEST1.
6. Save/reopen the project and verify that the path and selected instruments restore. See TESTING.md.

The test bank is not distributed with this project. Use your own legally obtained SoundFont. No Chmaha code is copied; the underlying engine is FluidSynth itself.

## Scope and limitations

Not yet compiled or tested on a Windows REAPER installation. SF3, MIDI timing, accessibility, host state restore and performance must be verified. FluidSynth renders internally in blocks, so sample-exact MIDI timing is not claimed. Loading may briefly use memory for two banks; no disk access occurs inside processBlock. The engine swap uses a non-blocking try-lock in processBlock: a contested block can be silent and its MIDI discarded. This is a known TEST1 limitation to improve before release. No independent FX controls, multi-outs, SFZ engine, search, custom shortcuts, or benchmark against BASSMIDI yet.

## Licensing

Development source for review. JUCE 8.0.12 is dual-licensed under AGPLv3 or the commercial JUCE licence. FluidSynth uses LGPL-2.1-or-later. Confirm your project licence and meet third-party source/relinking obligations before distributing binaries. An AGPLv3 open-source release is a possible route; commercial distribution under different terms may require a commercial JUCE licence. See THIRD_PARTY.md.
