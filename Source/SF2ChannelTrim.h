#pragma once
#include <cmath>

namespace lsfx {
// Temporary SF2 stereo-safe trim, expressed as an additive channel generator
// offset (centibels), not a post-mix fader. Preserve an external NRPN offset
// when it changes and restore it when the user trim returns to unity.
struct SF2ChannelTrim {
    float native = 0.0f;
    float written = 0.0f;
    bool engaged = false;

    template<class Read, class Write>
    bool update(float db, Read read, Write write) {
        // Most channels never use trim: leave the native generator untouched.
        if (!engaged && db == 0.0f) return false;
        const float current = read();
        if (!std::isfinite(current)) return false;
        if (!engaged || std::abs(current - written) > 0.0001f) native = current;
        const float target = native - db * 10.0f;
        const bool changed = std::abs(target - current) > 0.0001f;
        if (changed && !write(target)) return false;
        written = target;
        engaged = db != 0.0f;
        return changed;
    }
};
}
