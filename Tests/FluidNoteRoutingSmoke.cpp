// Real patched library smoke test: do not package the plugin if this fails.
#include <fluidsynth.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
extern "C" int lsfx_fluid_noteon(fluid_synth_t*,int,int,int,int,int,float,float,float);
extern "C" int lsfx_fluid_channel_dry(fluid_synth_t*,int,int);
static void check(bool ok,const char* text) {if(!ok)throw std::runtime_error(text);}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    try {
        constexpr int groups=128, frames=257;
        for(double rate:{44100.,48000.,96000.}) {
            std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);
            check(bool(settings),"settings");auto* st=settings.get();
            check(fluid_settings_setnum(st,"synth.sample-rate",rate)==FLUID_OK,"sample rate");
            for(const auto* name:{"synth.audio-channels","synth.audio-groups"})check(fluid_settings_setint(st,name,groups)==FLUID_OK,"128 groups");
            check(fluid_settings_setint(st,"synth.midi-channels",16)==FLUID_OK,"16 MIDI channels");
            check(fluid_settings_setint(st,"synth.effects-groups",1)==FLUID_OK,"one FX group");
            check(fluid_settings_setint(st,"synth.threadsafe-api",0)==FLUID_OK,"API mode");
            check(fluid_settings_setint(st,"synth.reverb.active",0)==FLUID_OK,"reverb off");
            check(fluid_settings_setint(st,"synth.chorus.active",0)==FLUID_OK,"chorus off");
            std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(st),delete_fluid_synth);
            check(bool(synth),"new synth");auto* s=synth.get();
            check(fluid_synth_count_audio_groups(s)==groups&&fluid_synth_count_audio_channels(s)==groups,"group count");
            const int font=fluid_synth_sfload(s,argv[1],0);check(font>=0,"SF2 load");
            check(fluid_synth_program_select(s,9,font,128,0)==FLUID_OK,"drum selection");
            std::array<std::array<float,frames>,2*groups> data{};
            std::array<float*,2*groups> ptr{};for(size_t i=0;i<ptr.size();++i)ptr[i]=data[i].data();
            std::array<std::array<float,frames>,4> fx{};std::array<float*,4> fp{};for(int i=0;i<4;++i)fp[i]=fx[i].data();
            std::array<float,groups> peak{};float wet=0;
            auto render=[&](int blocks) {
                peak.fill(0);wet=0;
                for(int block=0;block<blocks;++block) {
                    for(auto& d:data)d.fill(0);for(auto& f:fx)f.fill(0);
                    check(fluid_synth_process(s,frames,4,fp.data(),int(ptr.size()),ptr.data())==FLUID_OK,"128 group render");
                    for(int i=0;i<2*groups;++i)for(float x:data[i]) {check(std::isfinite(x),"finite dry");peak[i/2]=(std::max)(peak[i/2],std::abs(x));}
                    for(auto& f:fx)for(float x:f){check(std::isfinite(x),"finite wet");wet=(std::max)(wet,std::abs(x));}
                }
            };
            render(4); // silent render before first note, catches MSVC stack regressions
            check(lsfx_fluid_noteon(s,9,36,100,127,2,1.0f,0,0)==FLUID_OK,"explicit note route");
            check(lsfx_fluid_noteon(s,9,38,100,9,0,1.0f,0,0)==FLUID_OK,"inherited note route");
            render(16);check(peak[127]>1e-6f&&peak[9]>1e-6f,"same MIDI channel on two groups");
            for(int i=0;i<groups;++i)if(i!=9&&i!=127)check(peak[i]<1e-7f,"unexpected duplicate audio");
            check(fluid_synth_all_sounds_off(s,9)==FLUID_OK,"panic");render(8);
            // Explicit dry note, even with native send controllers turned up.
            check(fluid_synth_reverb_on(s,-1,1)==FLUID_OK,"reverb on");
            check(fluid_synth_chorus_on(s,-1,1)==FLUID_OK,"chorus on");
            check(fluid_synth_cc(s,9,91,127)==FLUID_OK,"CC91");check(fluid_synth_cc(s,9,93,127)==FLUID_OK,"CC93");
            check(lsfx_fluid_noteon(s,9,36,100,127,2,1.0f,0,0)==FLUID_OK,"dry note");
            render(40);check(peak[127]>1e-6f&&peak[9]<1e-7f&&wet<1e-7f,"isolated dry note with FX active");
            std::cout<<"PASS note routing at "<<rate<<" Hz\n";
        }
        return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
