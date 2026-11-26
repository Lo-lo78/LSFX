#include "../Source/OutputStage.h"
#include <cassert>
#include <iostream>
int main() {
  for(double rate : {44100.,48000.,96000.}) {
    lsfx::OutputStage stage; stage.prepare(rate);
    float l=0.01f,r=-0.01f;
    for(int i=0;i<5000;++i) { l=.01f; r=-.01f; stage.process(l,r,0.501187f,3.98107f); }
    assert(l>.031f && l<.033f); assert(l==-r);
    assert(lsfx::OutputStage::ceiling(.5f)==.5f);
    assert(lsfx::OutputStage::ceiling(.90f)==.90f);
    assert(lsfx::OutputStage::ceiling(.95f)>.90f);
    for(int i=0;i<2000;++i) { l=100.0f; r=-100.0f; stage.process(l,r,2.0f,15.85f); assert(std::isfinite(l)&&std::abs(l)<1.0f&&l==-r); }
    stage.reset();
    for(int i=0;i<5000;++i) { l=.1f; r=-.1f; stage.process(l,r,0.501187f,3.98107f,true); }
    assert(l>.318f && l<.320f && l==-r);
    for(int i=0;i<2000;++i) { l=100.f;r=-100.f;stage.process(l,r,2.f,15.85f,true);assert(std::isfinite(l)&&l<1.f&&l==-r); }
    // Reference mode must preserve amplitude, stereo balance and headroom.
    for(float x : {0.0001f,0.01f,0.25f,0.8f,1.4f,-0.8f}) {
        l=x; r=x*.37f; stage.processSoundFont(l,r,1,1,2);
        assert(l==x && r==x*.37f);
    }
    l=.1f; r=.2f; stage.processSoundFont(l,r,.5f,1,2); assert(l==.05f&&r==.1f);
    l=2; r=-2; stage.processSoundFont(l,r,1,1,1); assert(l<1&&l>.9f&&l==-r);
    stage.reset();
    for(int i=0;i<5000;++i) { l=.1f; r=-.05f; stage.processSoundFont(l,r,1,1,0); assert(l==.1f&&r==-.05f); }
    for(int i=0;i<5000;++i) { l=.5f; r=-.25f; stage.processSoundFont(l,r,1,1,0); }
    assert(std::abs(l-.3875f)<.0001f && std::abs(r+.19375f)<.0001f);
    l=0; r=0; stage.processSoundFont(l,r,1,1,2); assert(stage.envelope==0);
    lsfx::StereoDelay d; d.prepare(rate); float a=0,b=0;
    const int samples=int(rate*.1);
    for(int i=0;i<=samples*3;++i) {d.process(i==0?1.f:0.f,0,100,0,0,.5f,true,a,b); if(i==samples) assert(a>.99f&&b==0); if(i==samples*2) assert(b>.49f&&a==0); if(i==samples*3) assert(a>.24f&&b==0);}
    d.reset(); d.process(0,0,100,0,0,.5f,true,a,b); assert(a==0&&b==0);
    for(int i=0;i<samples*10;++i) {d.process(i==0?1:0,0,1,1,5,.85f,false,a,b); assert(std::isfinite(a)&&std::isfinite(b));}
  }
  std::cout<<"Output stage and stereo / ping-pong delay tests passed\n";
}
