#include "../Source/GridNavigation.h"
#include <cassert>
#include <iostream>
int main() {
    using lsfx::gridDestination; using M=lsfx::GridMove;
    assert(gridDestination(0,42,8,M::down)==1);
    assert(gridDestination(1,42,8,M::down)==2);
    assert(gridDestination(11,42,8,M::home)==8);
    assert(gridDestination(11,42,8,M::end)==15);
    assert(gridDestination(11,42,8,M::home,true)==0);
    assert(gridDestination(11,42,8,M::end,true)==41);
    assert(gridDestination(39,42,8,M::right)==41);
    assert(gridDestination(41,42,8,M::down)==41);
    assert(gridDestination(40,42,8,M::home)==40);
    assert(gridDestination(0,2,8,M::down)==1);
    assert(gridDestination(1,2,8,M::up)==0);
    // 128 entries: 8 columns of 16; the SF2 percussion bank is extra.
    assert(gridDestination(0,128,16,M::right)==16);
    assert(gridDestination(15,128,16,M::down)==15);
    assert(gridDestination(16,128,16,M::up)==16);
    assert(gridDestination(37,128,16,M::home)==32);
    assert(gridDestination(37,128,16,M::end)==47);
    assert(gridDestination(37,128,16,M::pageUp)==32);
    assert(gridDestination(37,128,16,M::pageDown)==42);
    assert(gridDestination(37,128,16,M::home,true)==0);
    assert(gridDestination(37,128,16,M::end,true)==127);
    assert(gridDestination(127,128,16,M::right)==127);
    assert(gridDestination(0,129,16,M::left)==0);
    assert(gridDestination(112,129,16,M::right)==128);
    assert(gridDestination(128,129,16,M::end)==128);
    assert(gridDestination(128,129,16,M::down)==128);
    using lsfx::channelDigitDestination;
    for(int from=0;from<16;++from) {
        for(int digit=2;digit<=9;++digit) assert(channelDigitDestination(from,digit)==digit-1);
        assert(channelDigitDestination(from,0)==from);
    }
    int ch=channelDigitDestination(4,1); assert(ch==0);
    for(int target=9;target<=15;++target) {ch=channelDigitDestination(ch,1);assert(ch==target);}
    assert(channelDigitDestination(ch,1)==0);
    std::cout<<"Grid navigation: all checks passed\n";
}
