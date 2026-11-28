#pragma once
// Virtual SoundFont: resident sample data and replaceable, precomputed zone
// descriptors. FluidSynth supplies all voice synthesis, interpolation and MIDI.
#include "InstrumentProject.h"
#include <fluidsynth.h>
#include <array>
namespace lsfx::instrument {
class FluidEditorBank {
    struct SampleDeleter {void operator()(fluid_sample_t* p)const {if(p)delete_fluid_sample(p);}};
    struct Asset {std::array<std::unique_ptr<fluid_sample_t,SampleDeleter>,2> pcm;int frames=0,channels=0;};
    struct Generators {std::array<std::pair<int,float>,56> data{};size_t count=0;void set(int id,int value){data[count++]={id,float(value)};}};
    struct ModDeleter {void operator()(fluid_mod_t* p)const {if(p)delete_fluid_mod(p);}};
    using Mods=std::vector<std::unique_ptr<fluid_mod_t,ModDeleter>>;
    static int flags(int s){return ((s>>8)&3)|((s>>8)&12)|((s&128)?FLUID_MOD_CC:0);}
    static std::shared_ptr<const Mods> makeMods(const Zone& z){auto mods=std::make_shared<Mods>();for(const auto& m:z.mods)if(!m.sfz){std::unique_ptr<fluid_mod_t,ModDeleter> v(new_fluid_mod());require(bool(v),"Cannot allocate modulator");fluid_mod_set_source1(v.get(),m.source&127,flags(m.source));fluid_mod_set_source2(v.get(),m.secondary&127,flags(m.secondary));fluid_mod_set_dest(v.get(),m.destination);fluid_mod_set_amount(v.get(),m.amount);fluid_mod_set_transform(v.get(),m.transform);mods->push_back(std::move(v));}return mods;}
    struct VoiceZone {int lo,hi,lv,hv;fluid_sample_t* sample;Generators gen;std::shared_ptr<const Mods> mods;};
    std::vector<Sample> sources;
    std::vector<Asset> assets;
    std::vector<VoiceZone> zones;
    static const char* bankName(fluid_sfont_t*){return "LSFX editor";}
    static const char* presetName(fluid_preset_t*){return "LSFX editor";}
    static int presetNumber(fluid_preset_t*){return 0;}
    static fluid_preset_t* getPreset(fluid_sfont_t* bank,int number,int program){
        if(number!=0||program!=0)return nullptr;
        auto* preset=new_fluid_preset(bank,presetName,presetNumber,presetNumber,noteOn,delete_fluid_preset);
        if(preset)fluid_preset_set_data(preset,fluid_sfont_get_data(bank));return preset;
    }
    static int noteOn(fluid_preset_t* preset,fluid_synth_t* synth,int channel,int key,int vel){
        auto* self=static_cast<FluidEditorBank*>(fluid_preset_get_data(preset));if(!self)return FLUID_FAILED;
        for(const auto& z:self->zones)if(key>=z.lo&&key<=z.hi&&vel>=z.lv&&vel<=z.hv){
            auto* voice=fluid_synth_alloc_voice(synth,z.sample,channel,key,vel);if(!voice)continue;
            for(size_t i=0;i<z.gen.count;++i)fluid_voice_gen_set(voice,z.gen.data[i].first,z.gen.data[i].second);
            for(const auto& m:*z.mods)fluid_voice_add_mod(voice,m.get(),FLUID_VOICE_OVERWRITE);
            fluid_synth_start_voice(synth,voice);
        }
        return FLUID_OK;
    }
    static int timecents(int ms,int limit=8000){return sfzexport::tc(ms/1000.0,limit);}
    static int frequency(int milliHz){return sfzexport::clampRound(1200*std::log2(std::max(.001,milliHz/1000.0)/8.176),-16000,4500);}
    static void offset(Generators& g,int fine,int coarse,int delta){g.set(fine,delta%32768);g.set(coarse,delta/32768);}
    static Generators generators(const Zone& z,const Asset& a,int ch){
        Generators g;const int last=a.frames-1;const int end=z.endFrame<0?last:z.endFrame;const int loopEnd=z.loopEnd<0?end:z.loopEnd;
        // Sample API uses a fixed full-length asset. Generator offsets are local
        // to the voice; neither PCM nor loop headers are ever mutated in place.
        offset(g,0,4,z.startFrame);offset(g,1,12,end-last);
        offset(g,2,45,z.loopStart);offset(g,3,50,loopEnd-last);
        g.set(58,z.root);int coarse=z.pitch/100;g.set(51,coarse);g.set(52,z.pitch-coarse*100);g.set(56,z.keyTrack);
        int att=-10*z.level;int pan=z.pan*5;if(a.channels==2){pan=ch==0?-500:500;att=std::min(1440,att+sfzexport::attenuation(1-std::max(0.0,(ch==0?z.pan:-z.pan)/100.0)));}
        g.set(17,pan);g.set(48,att);
        g.set(33,timecents(z.ampDelay,5000));g.set(34,timecents(z.ampAttack));g.set(35,timecents(z.ampHold,5000));g.set(36,timecents(z.ampDecay));g.set(37,sfzexport::attenuation(z.ampSustain/100.0));g.set(38,timecents(z.ampRelease));
        g.set(8,sfzexport::clampRound(1200*std::log2(z.cutoff/8.176),1500,13500));g.set(9,z.resonance);
        g.set(25,timecents(z.modDelay,5000));g.set(26,timecents(z.modAttack));g.set(27,timecents(z.modHold,5000));g.set(28,timecents(z.modDecay));g.set(29,1000-10*z.modSustain);g.set(30,timecents(z.modRelease));g.set(7,z.envPitch);g.set(11,z.envFilter);
        g.set(23,timecents(z.vibDelay,5000));g.set(24,frequency(z.vibFreq));g.set(6,z.vibPitch);
        g.set(21,timecents(z.lfoDelay,5000));g.set(22,frequency(z.lfoFreq));g.set(10,z.lfoFilter);g.set(13,z.lfoVolume);g.set(5,z.lfoPitch);g.set(39,z.ampHoldKey);g.set(40,z.ampDecayKey);g.set(31,z.modHoldKey);g.set(32,z.modDecayKey);
        g.set(54,z.loopMode==1?1:z.loopMode==2?3:0);g.set(57,z.choke);g.set(16,z.reverb*10);g.set(15,z.chorus*10);
        return g;
    }
public:
    FluidEditorBank(const Project& p,const std::function<sfzexport::Audio(const Sample&)>& decode):sources(p.samples){
        p.validate();assets.reserve(p.samples.size());size_t total=0;
        for(const auto& s:p.samples){auto audio=decode(s);require(audio.channels==1||audio.channels==2,"Invalid preview sample channels");Asset a;a.frames=int(audio.pcm.size()/size_t(audio.channels));a.channels=audio.channels;
            require(a.frames>0&&size_t(a.frames)*a.channels<=maxProjectBytes/2-total,"Decoded editor samples exceed preview RAM limit");total+=size_t(a.frames)*a.channels;
            std::vector<short> pcm(size_t(a.frames));
            for(int ch=0;ch<a.channels;++ch){for(int i=0;i<a.frames;++i){float v=audio.pcm[size_t(i)*a.channels+ch];require(std::isfinite(v),"Invalid preview PCM");pcm[size_t(i)]=short(sfzexport::clampRound(double(v)*32768,-32768,32767));}
                auto& slot=a.pcm[size_t(ch)];slot.reset(new_fluid_sample());require(bool(slot),"Cannot allocate editor sample");
                require(fluid_sample_set_sound_data(slot.get(),pcm.data(),nullptr,unsigned(a.frames),unsigned(audio.rate),1)==FLUID_OK,"Cannot copy editor PCM");
                // copy_data=true sets an eight-frame leading guard; loop end is
                // exclusive for the renderer. Offset generators reference this header.
                require(fluid_sample_set_loop(slot.get(),8,unsigned(8+a.frames))==FLUID_OK&&fluid_sample_set_pitch(slot.get(),s.root,0)==FLUID_OK,"Cannot configure editor sample");
            }assets.push_back(std::move(a));
        }
        replaceZones(p);
    }
    bool matches(const Project& p)const{return sameSamples(sources,p.samples);}
    // Same sample resources: only replace these tiny voice descriptors. No SF2
    // construction or sample decode. Already playing voices own their DSP state.
    using Prepared=std::vector<VoiceZone>;
    Prepared prepareZones(const Project& p)const{
        p.validate();Prepared next;next.reserve(p.zones.size()*2);
        for(const auto& z:p.zones){const auto& a=assets.at(size_t(z.sample));auto fixed=z;Sample bounds;bounds.frames=a.frames;clampFrames(fixed,bounds);
            auto mods=makeMods(fixed);for(int ch=0;ch<a.channels;++ch)next.push_back({fixed.low,fixed.high,fixed.lowVel,fixed.highVel,a.pcm[size_t(ch)].get(),generators(fixed,a,ch),mods});
        }
        return next;
    }
    void installZones(Prepared& next) noexcept{zones.swap(next);}
    void replaceZones(const Project& p){auto next=prepareZones(p);installZones(next);}
    void install(fluid_synth_t* synth){auto* bank=new_fluid_sfont(bankName,getPreset,nullptr,nullptr,delete_fluid_sfont);require(bank,"Cannot allocate virtual editor SoundFont");fluid_sfont_set_data(bank,this);const int id=fluid_synth_add_sfont(synth,bank);if(id<0){delete_fluid_sfont(bank);throw std::runtime_error("Cannot install virtual editor SoundFont");}require(fluid_synth_program_select(synth,0,id,0,0)==FLUID_OK,"Cannot select virtual editor SoundFont");}
};
}
