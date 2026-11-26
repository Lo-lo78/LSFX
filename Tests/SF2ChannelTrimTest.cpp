#include "../Source/SF2ChannelTrim.h"
#include <cassert>
#include <iostream>
int main() {
    lsfx::SF2ChannelTrim a,b;
    float nativeA=20, nativeB=0; int reads=0,writes=0;
    auto read=[&]{++reads;return nativeA;};
    auto write=[&](float v){++writes;nativeA=v;return true;};
    assert(!a.update(0,read,write)&&reads==0&&writes==0);
    assert(a.update(-6,read,write)&&nativeA==80&&writes==1);
    assert(!a.update(-6,read,write)&&writes==1);
    assert(a.update(6,read,write)&&nativeA==-40);
    assert(!b.update(0,[&]{return nativeB;},[&](float v){nativeB=v;return true;}));
    assert(nativeB==0);
    nativeA=10; // external generator update
    assert(a.update(6,read,write)&&nativeA==-50);
    assert(a.update(0,read,write)&&nativeA==10&&!a.engaged);
    assert(!a.update(0,read,write));
    assert(a.update(-12,read,write)&&nativeA==130);
    nativeA=0; // synth reset
    assert(a.update(-12,read,write)&&nativeA==120);
    assert(a.update(0,read,write)&&nativeA==0);
    std::cout<<"SF2ChannelTrim: all checks passed\n";
}
