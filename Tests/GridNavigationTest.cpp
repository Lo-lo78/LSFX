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
    // General MIDI: sixteen columns of eight entries, plus percussion bank.
    constexpr int rows=lsfx::locationGridRows;
    static_assert(rows==8);
    assert(gridDestination(0,128,rows,M::right)==8);
    assert(gridDestination(37,128,rows,M::home)==32);
    assert(gridDestination(37,128,rows,M::end)==39);
    assert(gridDestination(37,128,rows,M::pageUp)==32);
    assert(gridDestination(37,128,rows,M::pageDown)==39);
    assert(gridDestination(33,128,rows,M::pageDown)==38);
    assert(gridDestination(37,128,rows,M::home,true)==0);
    assert(gridDestination(37,128,rows,M::end,true)==127);
    for(int count: {128,129,130,16384}) {
        for(int i=0;i<count;++i) {
            int first=i-i%rows, last=std::min(first+rows-1,count-1);
            assert(gridDestination(i,count,rows,M::home)==first);
            assert(gridDestination(i,count,rows,M::end)==last);
            assert(gridDestination(i,count,rows,M::up)==std::max(first,i-1));
            assert(gridDestination(i,count,rows,M::down)==std::min(last,i+1));
            assert(gridDestination(i,count,rows,M::left)==(first>0?i-rows:i));
            assert(gridDestination(i,count,rows,M::right)==(first+rows<count?std::min(i+rows,count-1):i));
        }
    }
    assert(gridDestination(120,129,rows,M::right)==128);
    assert(gridDestination(128,129,rows,M::end)==128);
    assert(gridDestination(128,129,rows,M::down)==128);
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
