#pragma once
#include <array>
namespace lsfx {
// Extra mixer groups belong to a (logical MIDI channel, explicit destination)
// pair. Never reassign a group while a voice or its buffered tail may use it.
struct NoteRouteGroups {
    static constexpr int base=16, count=128;
    std::array<int,count> owner{},destination{};
    int used=base;
    NoteRouteGroups() noexcept {reset();}
    void reset() noexcept {owner.fill(-1);destination.fill(-1);used=base;}
    int acquire(int channel,int route) noexcept {
        if(channel<0||channel>=base||route<0||route>=64)return -1;
        for(int i=base;i<used;++i)if(owner[i]==channel&&destination[i]==route)return i;
        if(used==count)return -1;
        const int i=used++;owner[i]=channel;destination[i]=route;return i;
    }
};
}
