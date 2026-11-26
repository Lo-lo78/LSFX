// Independent native smoke test: two SF2 files, one stereo FluidSynth instance.
#include <fluidsynth.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
static void require(bool b,const char* msg) {if(!b) throw std::runtime_error(msg);}
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);
        require(bool(settings),"settings"); auto* st=settings.get();
        fluid_settings_setnum(st,"synth.sample-rate",44100);
        fluid_settings_setnum(st,"synth.gain",1);
        fluid_settings_setint(st,"synth.midi-channels",16);
        fluid_settings_setint(st,"synth.audio-channels",1);
        fluid_settings_setint(st,"synth.audio-groups",1);
        fluid_settings_setint(st,"synth.effects-groups",1);
        fluid_settings_setint(st,"synth.reverb.active",0);
        fluid_settings_setint(st,"synth.chorus.active",0);
        fluid_settings_setint(st,"synth.threadsafe-api",0);
        fluid_settings_setint(st,"synth.cpu-cores",1);
        fluid_settings_setstr(st,"synth.midi-bank-select","gs");
        std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(st),delete_fluid_synth);
        require(bool(synth),"synth"); auto* sy=synth.get();
        const int a=fluid_synth_sfload(sy,argv[1],1), b=fluid_synth_sfload(sy,argv[2],0);
        require(a>=0&&b>=0&&a!=b,"two source IDs");
        require(fluid_synth_program_select(sy,0,a,0,0)==FLUID_OK,"source A");
        require(fluid_synth_program_select(sy,1,b,128,0)==FLUID_OK,"source B drum preset");
        require(fluid_synth_bank_select(sy,1,1)==FLUID_OK,"virtual bank");
        int font=-1,bank=-1,program=-1;
        require(fluid_synth_get_program(sy,1,&font,&bank,&program)==FLUID_OK&&font==b&&bank==1,"bank select retains source ID");
        auto* preset=fluid_synth_get_channel_preset(sy,1);
        require(preset&&std::string(fluid_preset_get_name(preset))=="Drum test","mapped preset retained");
        auto render=[&] {
            float peak=0; std::array<float,1024> l{},r{};
            for(int i=0;i<4;++i) {
                require(fluid_synth_write_float(sy,1024,l.data(),0,1,r.data(),0,1)==FLUID_OK,"render");
                for(int k=0;k<1024;++k) {require(std::isfinite(l[k])&&std::isfinite(r[k]),"nonfinite"); peak=(std::max)(peak,(std::max)(std::abs(l[k]),std::abs(r[k])));}
            }
            return peak;
        };
        require(fluid_synth_noteon(sy,0,60,100)==FLUID_OK&&fluid_synth_noteon(sy,1,60,100)==FLUID_OK,"two source notes");
        require(render()>0.00001f,"mapped signal");
        for(int ch=0;ch<16;++ch) fluid_synth_all_sounds_off(sy,ch);
        require(fluid_synth_unset_program(sy,1)==FLUID_OK,"empty program");
        require(fluid_synth_bank_select(sy,1,1)==FLUID_OK,"empty bank retained");
        require(fluid_synth_noteon(sy,1,60,100)!=FLUID_OK,"empty slot cannot trigger previous source");
        render(); require(render()<0.00001f,"empty slot is silent");
        require(fluid_synth_cc(sy,1,0,5)==FLUID_OK,"MIDI bank select");
        require(fluid_synth_get_program(sy,1,&font,&bank,&program)==FLUID_OK&&bank==5,"pending GS bank");
        require(fluid_synth_program_select(sy,1,b,128,0)==FLUID_OK,"restore exact source");
        require(fluid_synth_bank_select(sy,1,5)==FLUID_OK,"restore virtual bank");
        require(fluid_synth_noteon(sy,1,60,100)==FLUID_OK&&render()>0.00001f,"restored signal");
        std::cout<<"PASS: two source IDs, virtual bank, silent empty program, restored source\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
