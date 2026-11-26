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
    std::cout<<"Grid navigation: all checks passed\n";
}
