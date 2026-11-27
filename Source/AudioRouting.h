#pragma once
namespace lsfx {
// Host destinations are independent of the sixteen MIDI/synth mixer groups.
inline constexpr int stereoOutputCount=64;
inline constexpr int maxOutputRoute=stereoOutputCount-1;
inline constexpr int monoOutputCount=2*stereoOutputCount;
}
