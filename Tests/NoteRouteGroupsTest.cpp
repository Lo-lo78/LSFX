#include "../Source/NoteRouteGroups.h"
#include "../Source/ChannelCompressor.h"
#include <cassert>
#include <cmath>
#include <array>
int main(){
    lsfx::NoteRouteGroups g;
    assert(g.acquire(9,1)==16&&g.acquire(9,1)==16&&g.acquire(9,2)==17);
    assert(g.acquire(1,1)==18&&g.owner[16]==9&&g.destination[16]==1);
    for(int r=0;r<64;++r)g.acquire(0,r);
    for(int r=0;r<64;++r)g.acquire(2,r);
    assert(g.used==128&&g.acquire(3,63)==-1);
    g.reset();assert(g.acquire(9,63)==16);
    lsfx::ChannelCompressor whole,split;whole.prepare(48000);split.prepare(48000);
    lsfx::CompValues c{};for(int i=0;i<lsfx::compCount;++i)c[i]=lsfx::compSpecs[i].initial;c[lsfx::compMix]=100;whole.configure(c);split.configure(c);
    std::array<float,1024> l{},r{},a{},b{},d{},e{};l.fill(.7f);r.fill(.4f);a.fill(.3f);b.fill(.1f);d.fill(.4f);e.fill(.3f);
    float* left[]={a.data(),d.data()};float* right[]={b.data(),e.data()};
    whole.process(l.data(),r.data(),1024);split.processSplit(left,right,2,1024);
    for(int i=0;i<1024;++i){assert(std::abs(a[i]+d[i]-l[i])<1e-6f);assert(std::abs(b[i]+e[i]-r[i])<1e-6f);}
}
