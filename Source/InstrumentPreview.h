#pragma once
// Isolated editor preview. The main virtual map is never mutated. All builds,
// sample decoding and engine destruction happen on a worker, not processBlock.
#include "InstrumentProjectIO.h"
#include "FluidEditorBank.h"
#include <fluidsynth.h>
#include <sfizz.h>
#include <cstring>
#include <cstdio>
#include <new>
#include <condition_variable>
#include <thread>
#include <chrono>
namespace lsfx::instrument {
struct PreviewEngine {
    fluid_settings_t* settings=nullptr;fluid_synth_t* fluid=nullptr;sfizz_synth_t* sfz=nullptr;
    std::unique_ptr<FluidEditorBank> bank;
    std::vector<Sample> sfzSources;std::string definition;juce::String virtualPath;double rate=44100;bool raw=false;
    sfzexport::Audio rawAudio;double rawPosition=0;bool rawPlaying=false;
    ~PreviewEngine(){if(sfz)sfizz_free(sfz);if(fluid)delete_fluid_synth(fluid);if(settings)delete_fluid_settings(settings);}
    void noteOn(int key,int vel){if(raw){rawPosition=0;rawPlaying=true;return;}if(fluid)fluid_synth_noteon(fluid,0,key,vel);else if(sfz)sfizz_send_note_on(sfz,0,key,vel);}
    void noteOff(int key,int vel){if(raw)return;if(fluid)fluid_synth_noteoff(fluid,0,key);else if(sfz)sfizz_send_note_off(sfz,0,key,vel);}
    void cc(int a,int v){if(raw){if(a==120||a==123)rawPlaying=false;return;}if(fluid)fluid_synth_cc(fluid,0,a,v);else if(sfz)sfizz_send_cc(sfz,0,a,v);}
    void stop(){rawPlaying=false;if(fluid)fluid_synth_all_sounds_off(fluid,0);else if(sfz)sfizz_all_sound_off(sfz);}
    void event(const uint8_t* d,int n){if(n<1)return;int status=d[0]&0xf0;if(status==0xf0)return;int a=n>1?d[1]&127:0,v=n>2?d[2]&127:0;
        if(status==0x90&&n>=3){if(v)noteOn(a,v);else noteOff(a,0);}else if(status==0x80&&n>=3)noteOff(a,v);
        else if(status==0xb0&&n>=3&&a!=0&&a!=32)cc(a,v);
        else if(status==0xe0&&n>=3){int bend=a|(v<<7);if(fluid)fluid_synth_pitch_bend(fluid,0,bend);else if(sfz)sfizz_send_hd_pitch_wheel(sfz,0,(bend-8192)/8192.0f);}
        else if(status==0xd0&&n>=2){if(fluid)fluid_synth_channel_pressure(fluid,0,a);else if(sfz)sfizz_send_channel_aftertouch(sfz,0,a);}
        else if(status==0xa0&&n>=3){if(fluid)fluid_synth_key_pressure(fluid,0,a,v);else if(sfz)sfizz_send_poly_aftertouch(sfz,0,a,v);}
    }
    void render(juce::AudioBuffer<float>& b,int start,int count){
        if(raw){if(!rawPlaying)return;const auto frames=rawAudio.pcm.size()/size_t(rawAudio.channels);auto* l=b.getWritePointer(0,start);auto* r=b.getWritePointer(1,start);const double step=double(rawAudio.rate)/rate;
            for(int i=0;i<count;++i){if(rawPosition>=double(frames)){rawPlaying=false;break;}auto a=size_t(rawPosition),z=(std::min)(a+1,frames-1);float f=float(rawPosition-double(a));auto read=[&](int ch){float x=rawAudio.pcm[a*rawAudio.channels+ch],y=rawAudio.pcm[z*rawAudio.channels+ch];return x+(y-x)*f;};l[i]=read(0);r[i]=read(rawAudio.channels-1);rawPosition+=step;}return;}
        for(int off=0;off<count;){int n=std::min(1024,count-off);auto* l=b.getWritePointer(0,start+off);auto* r=b.getWritePointer(1,start+off);
        if(fluid)fluid_synth_write_float(fluid,n,l,0,1,r,0,1);else if(sfz){float* out[]={l,r};sfizz_render_block(sfz,out,2,n);}off+=n;}}
    bool sfzMatches(const Project& p) const{return sfz&&sameSamples(sfzSources,p.samples);}
    void reloadSFZ(const Project& p){
        juce::AudioFormatManager formats;formats.registerBasicFormats();
        // Self-contained on every reload: sfizz 1.2.3 clears its sample pool
        // even for the same virtual path. Do not rely on old embedded IDs.
        auto text=sfzMemoryText(p,[&](const Sample& sample){return decode(formats,sample);});
        require(sfizz_load_string(sfz,virtualPath.toRawUTF8(),text.c_str()),"Cannot load SFZ editor preview");
        require(sfizz_get_num_regions(sfz)==int(p.zones.size()),"SFZ preview lost a region");
        sfzSources=p.samples;definition=sfzText(p);
    }
    static std::unique_ptr<PreviewEngine> build(Project p,double rate,int audition,std::unique_ptr<PreviewEngine>& standby){
        auto e=std::make_unique<PreviewEngine>();e->rate=rate;e->raw=audition>=0;
        if(audition>=0){require(size_t(audition)<p.samples.size(),"No sample to audition");juce::AudioFormatManager formats;formats.registerBasicFormats();e->rawAudio=decode(formats,p.samples[size_t(audition)]);e->rawPlaying=true;return e;}
        p.validate(true);
        if(p.format==Format::sf2){
            juce::AudioFormatManager formats;formats.registerBasicFormats();
            e->bank=std::make_unique<FluidEditorBank>(p,[&](const Sample& sample){return decode(formats,sample);});
            e->settings=new_fluid_settings();require(e->settings,"Cannot create preview settings");
            fluid_settings_setnum(e->settings,"synth.sample-rate",rate);fluid_settings_setnum(e->settings,"synth.gain",1.0);
            fluid_settings_setint(e->settings,"synth.polyphony",256);fluid_settings_setint(e->settings,"synth.dynamic-sample-loading",0);
            fluid_settings_setint(e->settings,"synth.reverb.active",0);fluid_settings_setint(e->settings,"synth.chorus.active",0);
            e->fluid=new_fluid_synth(e->settings);require(e->fluid,"Cannot create preview synth");
            e->bank->install(e->fluid);
            fluid_synth_cc(e->fluid,0,7,100);fluid_synth_cc(e->fluid,0,11,127);
        }else{
            // Reuse the inactive sfizz instance, not its cleared sample pool.
            // The new document includes all required audio; no files are created.
            if(standby&&standby->sfzMatches(p)&&std::abs(standby->rate-rate)<.01)e=std::move(standby);
            else {standby.reset();e->sfz=sfizz_create_synth();require(e->sfz,"Cannot create SFZ editor preview");sfizz_set_sample_rate(e->sfz,float(rate));sfizz_set_samples_per_block(e->sfz,1024);sfizz_set_num_voices(e->sfz,256);
                e->virtualPath=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("LSFX-RAM-"+juce::Uuid().toString()+".sfz").getFullPathName();}
            e->reloadSFZ(p);
        }
        return e;
    }
};
class InstrumentPreview {
    std::mutex requestMutex,audioMutex;std::condition_variable cv;std::thread worker;
    std::optional<Project> project;std::unique_ptr<PreviewEngine> live,parked;
    std::atomic<bool> enabled{false},kill{false},rawEnded{false},allowPrevious{false};std::atomic<uint64_t> wanted{0},published{0};
    double rate=44100;int audition=-1;bool stopping=false;juce::String status;
public:
    ~InstrumentPreview(){enabled.store(false);{std::lock_guard<std::mutex> g(requestMutex);stopping=true;}cv.notify_one();if(worker.joinable())worker.join();}
    bool active()const{return enabled.load();}
    void update(const Project& p,double r,int raw=-1){
        std::lock_guard<std::mutex> g(requestMutex);
        if(raw<0&&audition<0&&project&&std::abs(rate-r)<.01&&sameSound(*project,p)&&!status.startsWith("Preview error")){
            enabled.store(true);if(published.load()!=wanted.load())allowPrevious.store(true);return;
        }
        project=p;rate=r;audition=raw;kill.store(false);rawEnded.store(false);++wanted;allowPrevious.store(raw<0&&!p.zones.empty()&&enabled.load());enabled.store(true);status=p.zones.empty()&&raw<0?"Preview: import samples":"Preview: preparing";
        if(raw<0){
            std::string sfzDefinition;if(p.format==Format::sfz&&!p.zones.empty())sfzDefinition=sfzText(p);
            PreviewEngine* compatibleEngine=nullptr;
            {std::lock_guard<std::mutex> a(audioMutex);
                auto compatible=[&](const std::unique_ptr<PreviewEngine>& e){return e&&!e->raw&&std::abs(e->rate-r)<.01&&(p.format==Format::sf2?(e->bank&&e->bank->matches(p)):(e->sfzMatches(p)&&e->definition==sfzDefinition));};
                if(!compatible(live)&&compatible(parked))live.swap(parked);
                if(compatible(live))compatibleEngine=live.get();
            }
            // requestMutex prevents the worker from retiring compatibleEngine.
            // Prepare descriptor arrays while audio continues on the old table.
            if(compatibleEngine){
                try{
                    FluidEditorBank::Prepared next;if(compatibleEngine->bank)next=compatibleEngine->bank->prepareZones(p);
                    {std::lock_guard<std::mutex> a(audioMutex);if(compatibleEngine->bank)compatibleEngine->bank->installZones(next);published.store(wanted.load());}
                    status=p.zones.empty()?"Preview: import samples":"Preview: ready, play MIDI";cv.notify_one();return;
                }catch(const std::exception& e){status=juce::String("Preview error: ")+juce::String::fromUTF8(e.what());}
            }
        }
        if(!worker.joinable())worker=std::thread([this]{run();});cv.notify_one();
    }
    void setRate(double r){std::lock_guard<std::mutex> g(requestMutex);if(std::abs(rate-r)>0.01){rate=r;if(project){++wanted;status="Preview: preparing";cv.notify_one();}}}
    void disable(){std::lock_guard<std::mutex> g(requestMutex);enabled.store(false);allowPrevious.store(false);kill.store(false);
        // Suspend audio, not the draft. Restore the mapped engine if raw audition was active.
        PreviewEngine* mapped=nullptr;
        {std::lock_guard<std::mutex> a(audioMutex);if(live)live->stop();if(parked)parked->stop();if(audition>=0&&live&&live->raw&&parked)live.swap(parked);if(live&&!live->raw)mapped=live.get();}
        if(audition>=0&&project){audition=-1;++wanted;rawEnded.store(false);bool ready=false;
            if(mapped&&std::abs(mapped->rate-rate)<.01&&!project->zones.empty())try{
                if(project->format==Format::sf2&&mapped->bank&&mapped->bank->matches(*project)){auto next=mapped->bank->prepareZones(*project);std::lock_guard<std::mutex> a(audioMutex);mapped->bank->installZones(next);ready=true;}
                else if(project->format==Format::sfz&&mapped->sfzMatches(*project)&&mapped->definition==sfzText(*project))ready=true;
            }catch(...){ready=false;}
            if(ready){published.store(wanted.load());status="Preview: ready, play MIDI";}else status="Preview: preparing";
            cv.notify_one();
        }
    }
    void panic(){kill.store(true);}
    juce::String getStatus(){std::lock_guard<std::mutex> g(requestMutex);if(audition>=0&&rawEnded.load()&&published.load()==wanted.load())return "Preview: sample ended";return status;}
    bool render(juce::AudioBuffer<float>& b,juce::MidiBuffer& midi){
        if(!enabled.load())return false;
        std::unique_lock<std::mutex> lock(audioMutex,std::try_to_lock);
        if(lock.owns_lock()&&live&&(published.load()==wanted.load()||(allowPrevious.load()&&!live->raw))&&b.getNumChannels()>=2){
            if(kill.exchange(false))live->stop();int pos=0;
            for(const auto m:midi){int at=juce::jlimit(pos,b.getNumSamples(),m.samplePosition);live->render(b,pos,at-pos);live->event(m.data,m.numBytes);pos=at;}
            live->render(b,pos,b.getNumSamples()-pos);
            if(live->raw&&!live->rawPlaying)rawEnded.store(true);
        }
        midi.clear();return true;
    }
private:
    void run(){uint64_t seen=0;std::unique_ptr<PreviewEngine> standby;for(;;){std::optional<Project> p;double r=0;int raw=-1;uint64_t serial=0;
        {std::unique_lock<std::mutex> g(requestMutex);cv.wait(g,[&]{return stopping||wanted.load()!=seen;});if(stopping)return;
            serial=wanted.load();seen=serial;if(published.load()==serial)continue;p=project;r=rate;raw=audition;
            if(p&&raw<0){std::lock_guard<std::mutex> a(audioMutex);if(live&&live->raw&&parked){live.swap(parked);}}
}
        if(!p||p->format!=Format::sfz||p->zones.empty())standby.reset();std::unique_ptr<PreviewEngine> next;juce::String result="Preview: import samples";
        try{if(p&&(raw>=0||!p->zones.empty())){next=PreviewEngine::build(std::move(*p),r,raw,standby);result=raw>=0?"Preview: sample audition":"Preview: ready, play MIDI";}}catch(const std::exception& e){result=juce::String("Preview error: ")+juce::String::fromUTF8(e.what());}catch(...){result="Preview error: cannot build instrument";}
        std::unique_ptr<PreviewEngine> retired;
        {std::lock_guard<std::mutex> g(requestMutex);if(stopping)return;if(serial!=wanted.load()){if(next&&next->sfz)standby=std::move(next);continue;}
            {std::lock_guard<std::mutex> a(audioMutex);
                if(next&&next->raw&&live&&!live->raw){live->stop();retired.swap(parked);parked.swap(live);}
                else if(!next||!next->raw){retired.swap(parked);}
                if(next||!result.startsWith("Preview error")){live.swap(next);}rawEnded.store(false);published.store(serial);
            }status=result;
            }

        // Retired SFZ engine is a standby. Reload its complete embedded document
        // on the worker without touching the live pool.
        if(next&&next->sfz){next->stop();standby=std::move(next);}
    }}
};
}
