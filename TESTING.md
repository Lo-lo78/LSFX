# TEST1 checklist

- [ ] GitHub Actions: configure, compile and package Windows x64 VST3.
- [ ] REAPER scans the VST3; opening the UI does not crash.
- [ ] Load original FluidR3 GM.SF2. Expected 189 preset headers in this user's bank; no bank conversion.
- [ ] Channel 1 bank 0/program 0 plays a piano. Check bank 0 program 40, program 80 and a non-zero bank.
- [ ] Channel 10, bank 128 program 0: drum kit.
- [ ] Check Note Off 0x80 and Note On velocity zero, sustain CC64, pitch bend, channel pressure, CC7, CC10, CC11, CC0 and Program Change.
- [ ] Play MIDI on all 16 channels with distinct programs. Compare with another SF2 player using the same file, gain, interpolation and FX settings.
- [ ] NVDA: Tab/Shift+Tab through named controls, select bank/program by keyboard, use Read status, browse a path with spaces, focus after file dialog and after status dialog.
- [ ] Save project, close/reopen, confirm file and all selected programs restore. Save a REAPER preset, instantiate another LSoundFont and load the preset.
- [ ] Move/rename bank: show clear error, no wrong-bank playback after load attempt. Reopen original path.
- [ ] Change audio sample rate 44.1/48/96 kHz, check tuning, rendering, CPU and memory.
- [ ] Load second SF2 (different banks), rapid repeated load requests, close editor while loading, remove plugin during load.
- [ ] SF3: open a known valid SF3 test bank; not yet confirmed.
- [ ] Benchmark CPU, voice stealing, sonic quality vs BASSMIDI only after functional validation.
