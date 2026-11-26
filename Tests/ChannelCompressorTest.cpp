#include "../Source/ChannelCompressor.h"
#include <cassert>
#include <array>
#include <cmath>
#include <iostream>
using namespace lsfx;
int main() {
    CompValues p{};
    for(int i=0;i<compCount;++i) p[i]=compSpecs[i].initial;
    for(double rate:{44100.,48000.,96000.}) {
        std::array<ChannelCompressor,16> channels;
        for(auto& c:channels) { c.prepare(rate); c.configure(p); }
        std::array<float,256> l{},r{};
        l.fill(.5f); r.fill(.125f);
        channels[0].process(l.data(),r.data(),int(l.size()));
        assert(l[0]==.5f&&r[0]==.125f&&l.back()==.5f); // exact bypass
        auto compressed=p; compressed[compThreshold]=-30; compressed[compMakeup]=0; compressed[compMix]=100;
        channels[0].configure(compressed);
        for(int i=0;i<300;++i) { l.fill(.5f); r.fill(.125f); channels[0].process(l.data(),r.data(),int(l.size())); }
        // 4:1 at -30 dB; linked stereo and no change of pan.
        const float wet=l.back(), expected=.5f*std::pow(10.f,-(.75f*(20.f*std::log10(.5f)+30.f))/20.f);
        assert(std::abs(wet-expected)<.0002f);
        assert(std::abs(r.back()/wet-.25f)<.00001f);
        l.fill(.5f); r.fill(.125f); channels[1].process(l.data(),r.data(),int(l.size()));
        assert(l.back()==.5f&&r.back()==.125f); // independent detectors and gains
        compressed[compMix]=50; channels[0].configure(compressed);
        l.fill(.5f);r.fill(.125f);channels[0].process(l.data(),r.data(),int(l.size()));
        assert(std::abs(l.back()-(.5f+wet)*.5f)<.0002f); // true parallel wet/dry
        compressed[compMix]=0; channels[0].configure(compressed);
        l.fill(.5f);r.fill(.125f);channels[0].process(l.data(),r.data(),int(l.size()));
        assert(l.back()==.5f&&r.back()==.125f);
        compressed[compMix]=100; compressed[compRatio]=3; compressed[compAttack]=20; compressed[compRelease]=20;
        channels[2].configure(compressed);
        l.fill(1);r.fill(.5f);channels[2].process(l.data(),r.data(),int(l.size()));
        const auto initial=channels[2].over; assert(initial>0);
        for(int i=0;i<300;++i) {l.fill(0);r.fill(0);channels[2].process(l.data(),r.data(),int(l.size()));}
        assert(channels[2].over<.001f&&l.back()==0&&r.back()==0);
        channels[2].reset(); assert(channels[2].average==0&&channels[2].over==0);
    }
    std::cout<<"PASS: 16 independent stereo compressors, ratio, bypass, parallel mix, release, three sample rates\n";
}
