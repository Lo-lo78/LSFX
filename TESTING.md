# TEST4
1. Remove old LSoundFont VST3 from scan paths (same plugin ID); install LSFX. Scan REAPER.
2. Open FluidR3 GM.sf2. Play same MIDI, host sample rate and block size as TEST1. Compare REAPER Performance Meter at rest, 1 note, 4 notes, and full MIDI arrangement. Use generic UI to compare Fourth order / Seventh order and effects Off / Both. Compare dry sound separately from effects; no claim of equal wet sound.
3. Check sustain, note-off, program changes, channel 10 drums, panic and release. Verify silence after release with effects off; verify effects tails with effects on.
4. Open an SFZ with samples in its original relative directory. Play MIDI channel 1; test velocity layers, sustain, release, pitch wheel, CC, and sample offsets. Test switching from SF2 to SFZ and back, missing samples, save/reopen project. SFZ is single instrument, not 16 independent channels yet; aftertouch and complete SFZ opcode coverage not validated.
5. Test realtime and offline render, sample rate changes, host blocks larger than 1024. Confirm TEST1 state restoration and keyboard accessibility. No Windows compile or REAPER runtime tests performed here.
