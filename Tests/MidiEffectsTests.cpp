#include "../Source/MidiEffects.h"
#include <cassert>
#include <vector>
#include <array>
#include <iostream>
using K=lsfx::MidiFx;
struct Event { K k; int v,ch,cc; };
int main() {
    std::vector<Event> events;
    auto decode=[&](const std::vector<std::uint8_t>& bytes) { events.clear(); return lsfx::decodeMidiEffects(bytes.data(),int(bytes.size()),[&](K k,int v,int ch,int cc){events.push_back({k,v,ch,cc});}); };
    auto gs=[](int mid,int addr,std::initializer_list<int> values){std::vector<std::uint8_t> b{0xf0,0x41,0x10,0x42,0x12,0x40,std::uint8_t(mid),std::uint8_t(addr)};int sum=0x40+mid+addr;for(auto v:values){b.push_back(std::uint8_t(v));sum+=v;}b.push_back(std::uint8_t((128-sum%128)%128));b.push_back(0xf7);return b;};
    for(int type=0;type<8;++type){assert(decode(gs(1,0x30,{type})));assert(events[0].k==K::reverbType&&events[0].v==type);assert(decode(gs(1,0x38,{type})));assert(events[0].k==K::chorusType&&events[0].v==type);}
    assert(decode(gs(1,0x33,{127,64,0})));assert(events.size()==3&&events[0].v==100&&events[1].v==50);
    for(int part=0;part<16;++part){assert(decode(gs(0x10+part,0x21,{60,90})));assert(events.size()==2&&events[0].cc==93&&events[1].cc==91);assert(events[0].ch==(part==0?9:part<=9?part-1:part));}
    auto bad=gs(1,0x38,{3});bad[bad.size()-2]^=1;assert(!decode(bad)&&events.empty());
    assert(decode({0xf0,0x43,0x10,0x4c,2,1,0,4,0,0xf7}));assert(events[0].k==K::reverbType&&events[0].v==5);
    assert(decode({0xf0,0x43,0x10,0x4c,2,1,0x20,0,0,0xf7}));assert(events[0].k==K::chorusEnable&&events[0].v==0);
    assert(decode({0xf0,0x43,0x10,0x4c,8,15,0x13,127,0xf7}));assert(events[0].ch==15&&events[0].cc==91);
    assert(decode({0xf0,0x7f,0x7f,4,1,127,127,0xf7}));assert(events[0].k==K::masterVolume&&events[0].v==16383);
    assert(decode({0xf0,0x7e,0x7f,9,1,0xf7}));assert(events[0].k==K::reset);
    assert(decode(gs(0,0x7f,{0})));assert(events[0].k==K::reset);
    assert(!decode({0xf0,0x43,0x10,0x4c,2,1,0,0xf7}));assert(!decode({0xf0,0x7f,0x7f,4,1,127,127}));
    std::cout<<"MIDI FX parser: all tests passed\n";
}
