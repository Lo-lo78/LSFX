#include "../Source/FluidEditorBank.h"
#include "../Source/InstrumentSourceImport.h"
#include <iostream>
#include <chrono>
using namespace lsfx::instrument;
static void compareImportedLevel(){
    const auto dir=std::filesystem::temp_directory_path()/("lsfx-gain-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directory(dir);
    struct Cleanup{Path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{dir};
    auto source=dir/"original.sf2",exported=dir/"exported.sf2";
    auto audio=[](const Path&){lsfx::sfzexport::Audio a;a.rate=44100;a.channels=1;for(int i=0;i<16384;++i)a.pcm.push_back(.25f*std::sin(i*.1f));return a;};
    // Independent ordinary converter: native SF2 generator 309 centibels, not
    // the editor's effective dB unit. FluidSynth should apply its 0.4 scale.
    lsfx::sfzexport::Region reg{{{"sample","fixture.wav"},{"volume","-30.9"},{"ampeg_attack","0.027"},{"ampeg_sustain","67.8"},{"ampeg_decay","0.06"}},dir};
    lsfx::sfzexport::Report report;lsfx::sfzexport::convertRegions({reg},"Gain test",source,audio,report);
    auto p=importSF2Program(source,0,0).project;require(p.zones[0].level==-1236,"Imported attenuation is not in audible dB");
    auto decode=[](const Sample& s){lsfx::sfzexport::Audio a;a.rate=44100;a.channels=1;const auto& b=*s.bytes;for(size_t i=44;i+1<b.size();i+=2)a.pcm.push_back(signed16(u16(b,i))/32768.0f);return a;};
    lsfx::sfzexport::Report ex;lsfx::sfzexport::convertRegions(regions(p),p.name,exported,[&](const Path&){return decode(p.samples[0]);},ex);
    FluidEditorBank bank(p,decode);
    auto measure=[&](const Path* path,int velocity){
        std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);fluid_settings_setnum(settings.get(),"synth.sample-rate",44100);fluid_settings_setnum(settings.get(),"synth.gain",1.0);fluid_settings_setint(settings.get(),"synth.reverb.active",0);fluid_settings_setint(settings.get(),"synth.chorus.active",0);
        std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(settings.get()),delete_fluid_synth);
        if(path){auto id=fluid_synth_sfload(synth.get(),path->u8string().c_str(),1);require(id>=0&&fluid_synth_program_select(synth.get(),0,id,0,0)==FLUID_OK,"Native gain test load failed");}else bank.install(synth.get());
        fluid_synth_cc(synth.get(),0,7,100);fluid_synth_cc(synth.get(),0,11,127);fluid_synth_noteon(synth.get(),0,60,velocity);
        std::vector<float> l(8192),r(8192);fluid_synth_write_float(synth.get(),int(l.size()),l.data(),0,1,r.data(),0,1);double square=0,peak=0;for(size_t i=0;i<l.size();++i){square+=double(l[i])*l[i]+double(r[i])*r[i];peak=std::max({peak,double(std::abs(l[i])),double(std::abs(r[i]))});}fluid_synth_all_sounds_off(synth.get(),0);return std::array<double,2>{std::sqrt(square/(2*l.size())),peak};
    };
    for(int v:{32,64,100,127}){auto original=measure(&source,v),editor=measure(nullptr,v),round=measure(&exported,v);require(original[0]>1e-6,"Native comparison silent");double worst=0;for(size_t i=0;i<2;++i){double de=20*std::log10(editor[i]/original[i]),dr=20*std::log10(round[i]/original[i]);worst=std::max({worst,std::abs(de),std::abs(dr)});require(std::abs(de)<.25&&std::abs(dr)<.25,"SF2 imported/exported RMS or peak differs by more than 0.25 dB");}std::cout<<"PASS: SF2 original/editor/export velocity "<<v<<", max RMS/peak difference "<<worst<<" dB\n";}
}
int main(){try{compareImportedLevel();
    Project p;auto blob=std::make_shared<Blob>(1024,23);p.addSample({"Stereo.wav","wav",blob,0,60,44100});p.addZones({0},Mapping::chromatic,60);
    int decodes=0;FluidEditorBank bank(p,[&](const Sample&){++decodes;lsfx::sfzexport::Audio a;a.rate=44100;a.channels=2;for(int i=0;i<44100;++i){a.pcm.push_back(.4f*std::sin(i*.1f));a.pcm.push_back(.2f*std::sin(i*.17f));}return a;});
    std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);fluid_settings_setnum(settings.get(),"synth.sample-rate",44100);fluid_settings_setint(settings.get(),"synth.reverb.active",0);fluid_settings_setint(settings.get(),"synth.chorus.active",0);
    std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(settings.get()),delete_fluid_synth);bank.install(synth.get());
    auto render=[&](int count){std::vector<float> l(count),r(count);fluid_synth_write_float(synth.get(),count,l.data(),0,1,r.data(),0,1);std::array<float,2> peak{};for(int i=0;i<count;++i){peak[0]=std::max(peak[0],std::abs(l[i]));peak[1]=std::max(peak[1],std::abs(r[i]));}return peak;};
    auto reset=[&]{fluid_synth_all_sounds_off(synth.get(),0);render(256);};
    fluid_synth_noteon(synth.get(),0,60,100);auto a=render(2048);require(a[0]>.001&&a[1]>.001,"Resident SF2 stereo note silent");
    auto z=p.zones[0];z.low=z.high=62;z.pan=-100;z.loopMode=1;z.loopStart=100;z.loopEnd=2000;z.startFrame=50;z.endFrame=2200;p.zones[0]=z;bank.replaceZones(p);require(decodes==1&&bank.matches(p),"Editing redecoded samples");
    require(render(2048)[0]>.001,"Editing interrupted active note");reset();fluid_synth_noteon(synth.get(),0,60,100);require(render(1024)[0]<1e-6,"Previous key mapping still active");fluid_synth_noteon(synth.get(),0,62,100);a=render(16384);require(a[0]>.001&&a[1]<a[0]*.05,"Edited stereo pan/mapping invalid");require(render(16384)[0]>.001,"Loop did not sustain");
    fluid_synth_noteoff(synth.get(),0,62);render(8192);require(render(2048)[0]<1e-5,"Note off failed");reset();
    z.loopMode=0;z.level=-2400;z.ampAttack=300;z.cutoff=800;z.choke=1;p.zones[0]=z;bank.replaceZones(p);fluid_synth_noteon(synth.get(),0,62,100);require(render(128)[0]<a[0]*.1,"Edited envelope/level not applied");reset();
    z.pan=0;z.level=0;z.ampAttack=0;z.cutoff=19912;z.loopMode=1;z.mods={{128+74,48,960,0,0,false}};p.zones[0]=z;bank.replaceZones(p);
    fluid_synth_cc(synth.get(),0,74,0);fluid_synth_noteon(synth.get(),0,62,100);auto loud=render(4096);require(loud[0]>.001,"Custom mod baseline silent");fluid_synth_cc(synth.get(),0,74,127);auto quiet=render(8192);quiet=render(4096);require(quiet[0]<loud[0]*.05,"Custom CC did not modulate held SF2 note");reset();
    p.zones.clear();bank.replaceZones(p);fluid_synth_noteon(synth.get(),0,62,100);require(render(2048)[0]<1e-6,"Empty mapping still produces audio");
    std::cout<<"PASS: resident virtual SF2 sample stereo, mapping updates without decode, held notes, loop, pan, envelope, filter, empty map\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
