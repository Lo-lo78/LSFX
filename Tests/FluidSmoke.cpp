// Standalone executable using the same linked FluidSynth as the Windows VST3.
// A native access violation is confined to this process, not the DAW.
#include <fluidsynth.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
void stage(const char* message) { std::cout << message << std::endl; }
void run(const char* path, bool multi, double rate) {
    constexpr int channels=16, chunk=1024;
    std::cout << "FluidSynth " << fluid_version_str() << "; " << (multi?"multigroup":"stereo") << "; " << rate << std::endl;
    std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);
    require(bool(settings),"settings failed"); auto* st=settings.get();
    const int groups=multi?channels:1;
    stage("configure settings");
    require(fluid_settings_setnum(st,"synth.sample-rate",rate)==FLUID_OK,"sample rate");
    require(fluid_settings_setnum(st,"synth.gain",1)==FLUID_OK,"gain");
    const std::pair<const char*,int> ints[]={{"synth.midi-channels",16},{"synth.audio-channels",groups},{"synth.audio-groups",groups},{"synth.effects-groups",1},{"synth.polyphony",256},{"synth.reverb.active",0},{"synth.chorus.active",0},{"synth.threadsafe-api",0},{"synth.cpu-cores",1}};
    for(auto kv:ints) require(fluid_settings_setint(st,kv.first,kv.second)==FLUID_OK,kv.first);
    require(fluid_settings_setstr(st,"synth.midi-bank-select","gs")==FLUID_OK,"bank select");
    stage("new_fluid_synth");
    std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(st),delete_fluid_synth);
    require(bool(synth),"new synth failed"); auto* sy=synth.get();
    require(fluid_synth_count_audio_channels(sy)==groups,"unexpected channel count");
    require(fluid_synth_count_audio_groups(sy)==groups,"unexpected group count");
    require(fluid_synth_count_effects_groups(sy)==1,"unexpected FX group count");
    stage("fluid_synth_sfload");
    const int font=fluid_synth_sfload(sy,path,1); require(font>=0,"sfload failed");
    stage("interpolation, preset enumeration and program selection");
    require(fluid_synth_set_interp_method(sy,-1,FLUID_INTERP_4THORDER)==FLUID_OK,"interpolation");
    auto* sf=fluid_synth_get_sfont_by_id(sy,font); require(sf!=nullptr,"font lookup");
    fluid_sfont_iteration_start(sf); int presets=0;
    while(auto* p=fluid_sfont_iteration_next(sf)) { (void)fluid_preset_get_name(p); ++presets; }
    require(presets>=2,"fixture presets");
    for(int ch=0;ch<channels;++ch) require(fluid_synth_program_select(sy,ch,font,ch==9?128:0,0)==FLUID_OK,"program selection");
    std::array<std::array<float,chunk>,2*channels> dry{};
    std::array<std::array<float,chunk>,4> wet{};
    std::array<float*,2*channels> dp{}; std::array<float*,4> wp{};
    for(size_t i=0;i<dp.size();++i) dp[i]=dry[i].data();
    for(size_t i=0;i<wp.size();++i) wp[i]=wet[i].data();
    auto render=[&](int count) {
        float peak=0;
        for(int left=count;left>0;) {
            const int n=(std::min)(left,chunk); left-=n;
            for(auto& d:dry) d.fill(0);
            for(auto& w:wet) w.fill(0);
            if(multi) require(fluid_synth_process(sy,n,4,wp.data(),32,dp.data())==FLUID_OK,"process failed");
            else require(fluid_synth_write_float(sy,n,dp[0],0,1,dp[1],0,1)==FLUID_OK,"write_float failed");
            for(const auto& d:dry) for(int i=0;i<n;++i) {require(std::isfinite(d[i]),"non-finite sample");peak=(std::max)(peak,std::abs(d[i]));}
            for(const auto& w:wet) for(int i=0;i<n;++i) require(std::isfinite(w[i]),"non-finite FX sample");
        }
        return peak;
    };
    for(int mode:{0,3,0,3}) {
        std::cout << "configure FX mode " << mode << std::endl;
        require(fluid_synth_reverb_on(sy,-1,mode&1)==FLUID_OK,"reverb toggle");
        require(fluid_synth_chorus_on(sy,-1,(mode&2)!=0)==FLUID_OK,"chorus toggle");
        require(fluid_synth_set_reverb_group_roomsize(sy,-1,.50)==FLUID_OK,"reverb roomsize");
        require(fluid_synth_set_reverb_group_damp(sy,-1,.30)==FLUID_OK,"reverb damping");
        require(fluid_synth_set_reverb_group_width(sy,-1,.80)==FLUID_OK,"reverb width");
        require(fluid_synth_set_reverb_group_level(sy,-1,.70)==FLUID_OK,"reverb level");
        require(fluid_synth_set_chorus_group_nr(sy,-1,4)==FLUID_OK,"chorus count");
        require(fluid_synth_set_chorus_group_level(sy,-1,.55)==FLUID_OK,"chorus level");
        require(fluid_synth_set_chorus_group_speed(sy,-1,.36)==FLUID_OK,"chorus speed");
        require(fluid_synth_set_chorus_group_depth(sy,-1,3.6)==FLUID_OK,"chorus depth");
        stage("first silent rendering");
        for(int n:{1,63,64,65,127,512,1024,2049}) {
            std::cout << "  silent frames=" << n << std::endl;
            render(n);
        }
        stage("MIDI notes and rendering");
        for(int ch=0;ch<channels;++ch) {
            require(fluid_synth_cc(sy,ch,91,40)==FLUID_OK,"CC91");
            require(fluid_synth_cc(sy,ch,93,40)==FLUID_OK,"CC93");
            if(multi&&ch>0) {
                require(fluid_synth_set_gen(sy,ch,GEN_REVERBSEND,-10000)==FLUID_OK,"reverb suppression");
                require(fluid_synth_set_gen(sy,ch,GEN_CHORUSSEND,-10000)==FLUID_OK,"chorus suppression");
            }
            require(fluid_synth_noteon(sy,ch,ch==9?36:60,100)==FLUID_OK,"noteon");
        }
        float peak=0;
        for(int n:{1,63,127,511,1024,2049,512}) peak=(std::max)(peak,render(n));
        require(peak>0,"no signal");
        for(int ch=0;ch<channels;++ch) require(fluid_synth_all_sounds_off(sy,ch)==FLUID_OK,"all sounds off");
    }
    stage("delete_fluid_synth"); synth.reset();
    stage("PASS");
}
}
int main(int argc,char** argv) {
    if(argc!=3||(std::string(argv[2])!="stereo"&&std::string(argv[2])!="multi")) return 2;
    try { for(double rate:{44100.,48000.,96000.}) run(argv[1],std::string(argv[2])=="multi",rate); }
    catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<std::endl; return 1;}
}
