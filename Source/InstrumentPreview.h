#pragma once
// Isolated editor preview. The main virtual map is never mutated. All builds,
// sample decoding and engine destruction happen on a worker, not processBlock.
#include "InstrumentProjectIO.h"
#include <fluidsynth.h>
#include <sfizz.h>
#include <cstring>
#include <cstdio>
#include <new>
#include <condition_variable>
#include <thread>
#include <chrono>
namespace lsfx::instrument {
struct PreviewMemory {
    struct Cursor {const std::string* data;size_t pos=0;};
    // The default SF2 loader runs synchronously. Dynamic sample loading is off.
    inline static thread_local const std::string* loading=nullptr;
    inline static thread_local const char* token=nullptr;
    static void* open(const char* name) noexcept {
        if(!loading||!token||!name||std::strcmp(name,token)!=0)return nullptr;
        return new(std::nothrow) Cursor{loading,0};
    }
    static int read(void* buf,fluid_long_long_t count,void* handle) noexcept {
        auto& c=*static_cast<Cursor*>(handle);if(count<0||uint64_t(count)>c.data->size()-c.pos)return FLUID_FAILED;
        if(count)std::memcpy(buf,c.data->data()+c.pos,size_t(count));c.pos+=size_t(count);return FLUID_OK;
    }
    static int seek(void* handle,fluid_long_long_t offset,int origin) noexcept {
        auto& c=*static_cast<Cursor*>(handle);fluid_long_long_t base=0;
        if(origin==SEEK_CUR)base=fluid_long_long_t(c.pos);else if(origin==SEEK_END)base=fluid_long_long_t(c.data->size());else if(origin!=SEEK_SET)return FLUID_FAILED;
        if(offset< -base||offset>fluid_long_long_t(c.data->size())-base)return FLUID_FAILED;
        c.pos=size_t(base+offset);return FLUID_OK;
    }
    static fluid_long_long_t tell(void* handle) noexcept {return fluid_long_long_t(static_cast<Cursor*>(handle)->pos);}
    static int close(void* handle) noexcept {delete static_cast<Cursor*>(handle);return FLUID_OK;}
};
struct PreviewCache {
    juce::File dir;std::vector<Sample> samples;
    ~PreviewCache(){if(dir!=juce::File{})dir.deleteRecursively();}
    bool matches(const Project& p)const{if(samples.size()!=p.samples.size())return false;for(size_t i=0;i<samples.size();++i)if(samples[i].bytes!=p.samples[i].bytes||samples[i].extension!=p.samples[i].extension)return false;return true;}
    static std::shared_ptr<PreviewCache> create(const Project& p){
        auto c=std::make_shared<PreviewCache>();auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("LSFX-preview-"+juce::Uuid().toString());
        require(dir.createDirectory().wasOk(),"Cannot create SFZ preview cache");c->dir=dir;
        auto samples=dir.getChildFile("Samples");require(samples.createDirectory().wasOk(),"Cannot create preview sample cache");
        for(size_t i=0;i<p.samples.size();++i){auto& s=p.samples[i];writeNew(pathOf(samples.getChildFile(js(sampleFilename(i,s)))),s.bytes->data(),s.bytes->size());}
        c->samples=p.samples;return c;
    }
};
struct PreviewEngine {
    fluid_settings_t* settings=nullptr;fluid_synth_t* fluid=nullptr;sfizz_synth_t* sfz=nullptr;
    std::shared_ptr<PreviewCache> cache;double rate=44100;bool raw=false;
    sfzexport::Audio rawAudio;double rawPosition=0;bool rawPlaying=false;
    ~PreviewEngine(){if(sfz)sfizz_free(sfz);if(fluid)delete_fluid_synth(fluid);if(settings)delete_fluid_settings(settings);}
    void noteOn(int key,int vel){if(raw){rawPosition=0;rawPlaying=true;return;}if(fluid)fluid_synth_noteon(fluid,0,key,vel);else if(sfz)sfizz_send_note_on(sfz,0,key,vel);}
    void noteOff(int key,int vel){if(raw)return;if(fluid)fluid_synth_noteoff(fluid,0,key);else if(sfz)sfizz_send_note_off(sfz,0,key,vel);}
    void cc(int a,int v){if(raw){if(a==120||a==123)rawPlaying=false;return;}if(fluid)fluid_synth_cc(fluid,0,a,v);else if(sfz)sfizz_send_cc(sfz,0,a,v);}
    void stop(){rawPlaying=false;if(fluid)fluid_synth_all_sounds_off(fluid,0);else if(sfz)sfizz_all_sound_off(sfz);}
    void event(const uint8_t* d,int n){if(n<1)return;int status=d[0]&0xf0;if(status==0xf0)return;int a=n>1?d[1]&127:0,v=n>2?d[2]&127:0;
        if(status==0x90&&n>=3){if(v)noteOn(a,v);else noteOff(a,0);}else if(status==0x80&&n>=3)noteOff(a,v);
        else if(status==0xb0&&n>=3&&a!=0&&a!=32&&a!=91&&a!=93)cc(a,v);
        else if(status==0xe0&&n>=3){int bend=a|(v<<7);if(fluid)fluid_synth_pitch_bend(fluid,0,bend);else if(sfz)sfizz_send_hd_pitch_wheel(sfz,0,(bend-8192)/8192.0f);}
        else if(status==0xd0&&n>=2){if(fluid)fluid_synth_channel_pressure(fluid,0,a);else if(sfz)sfizz_send_channel_aftertouch(sfz,0,a);}
        else if(status==0xa0&&n>=3){if(fluid)fluid_synth_key_pressure(fluid,0,a,v);else if(sfz)sfizz_send_poly_aftertouch(sfz,0,a,v);}
    }
    void render(juce::AudioBuffer<float>& b,int start,int count){
        if(raw){if(!rawPlaying)return;const auto frames=rawAudio.pcm.size()/size_t(rawAudio.channels);auto* l=b.getWritePointer(0,start);auto* r=b.getWritePointer(1,start);const double step=double(rawAudio.rate)/rate;
            for(int i=0;i<count;++i){if(rawPosition>=double(frames)){rawPlaying=false;break;}auto a=size_t(rawPosition),z=(std::min)(a+1,frames-1);float f=float(rawPosition-double(a));auto read=[&](int ch){float x=rawAudio.pcm[a*rawAudio.channels+ch],y=rawAudio.pcm[z*rawAudio.channels+ch];return x+(y-x)*f;};l[i]=read(0);r[i]=read(rawAudio.channels-1);rawPosition+=step;}return;}
        for(int off=0;off<count;){int n=std::min(1024,count-off);auto* l=b.getWritePointer(0,start+off);auto* r=b.getWritePointer(1,start+off);
        if(fluid)fluid_synth_write_float(fluid,n,l,0,1,r,0,1);else if(sfz){float* out[]={l,r};sfizz_render_block(sfz,out,2,n);}off+=n;}}
    static std::unique_ptr<PreviewEngine> build(Project p,double rate,int audition,std::shared_ptr<PreviewCache>& cached){
        auto e=std::make_unique<PreviewEngine>();e->rate=rate;e->raw=audition>=0;
        if(audition>=0){require(size_t(audition)<p.samples.size(),"No sample to audition");juce::AudioFormatManager formats;formats.registerBasicFormats();e->rawAudio=decode(formats,p.samples[size_t(audition)]);e->rawPlaying=true;return e;}
        p.validate(true);
        if(p.format==Format::sf2){
            // Exactly the same generator as final SF2 export, but backed by RAM.
            juce::AudioFormatManager formats;formats.registerBasicFormats();std::map<Path,size_t> indices;
            for(size_t i=0;i<p.samples.size();++i)indices.emplace(std::filesystem::u8path(sampleFilename(i,p.samples[i])),i);
            std::ostringstream stream(std::ios::out|std::ios::binary);sfzexport::Report report;
            sfzexport::convertRegionsToStream(regions(p),p.name,stream,[&](const Path& path){return decode(formats,p.samples.at(indices.at(path)));},report);
            require(report.regions==p.zones.size()&&report.skipped==0,"Preview lost mapping zones");auto bytes=stream.str();
            e->settings=new_fluid_settings();require(e->settings,"Cannot create preview settings");
            fluid_settings_setnum(e->settings,"synth.sample-rate",rate);fluid_settings_setnum(e->settings,"synth.gain",0.8);
            fluid_settings_setint(e->settings,"synth.polyphony",256);fluid_settings_setint(e->settings,"synth.dynamic-sample-loading",0);
            fluid_settings_setint(e->settings,"synth.reverb.active",0);fluid_settings_setint(e->settings,"synth.chorus.active",0);
            e->fluid=new_fluid_synth(e->settings);require(e->fluid,"Cannot create preview synth");
            auto* loader=new_fluid_defsfloader(e->settings);require(loader,"Cannot create memory loader");
            if(fluid_sfloader_set_callbacks(loader,PreviewMemory::open,PreviewMemory::read,PreviewMemory::seek,PreviewMemory::tell,PreviewMemory::close)!=FLUID_OK){delete_fluid_sfloader(loader);throw std::runtime_error("Cannot configure memory loader");}
            fluid_synth_add_sfloader(e->fluid,loader);
            static std::atomic<uint64_t> serial{0};auto token=std::string("LSFX-memory-")+std::to_string(++serial)+".sf2";
            PreviewMemory::loading=&bytes;PreviewMemory::token=token.c_str();int id=fluid_synth_sfload(e->fluid,token.c_str(),0);PreviewMemory::loading=nullptr;PreviewMemory::token=nullptr;
            require(id>=0&&fluid_synth_program_select(e->fluid,0,id,0,0)==FLUID_OK,"Cannot load SF2 editor preview");
            fluid_synth_cc(e->fluid,0,7,100);fluid_synth_cc(e->fluid,0,11,127);
        }else{
            // sfizz 1.2.3 reads SFZ text from RAM but resolves audio via files.
            // Only a private scratch cache is written; never export or touch originals.
            if(!cached||!cached->matches(p))cached=PreviewCache::create(p);e->cache=cached;auto dir=cached->dir;
            e->sfz=sfizz_create_synth();require(e->sfz,"Cannot create SFZ editor preview");sfizz_set_sample_rate(e->sfz,float(rate));sfizz_set_samples_per_block(e->sfz,1024);sfizz_set_num_voices(e->sfz,128);
            auto virtualPath=dir.getChildFile("Preview.sfz").getFullPathName();auto text=sfzText(p);require(sfizz_load_string(e->sfz,virtualPath.toRawUTF8(),text.c_str()),"Cannot load SFZ editor preview");
        }
        return e;
    }
};
class InstrumentPreview {
    std::mutex requestMutex,audioMutex;std::condition_variable cv;std::thread worker;
    std::optional<Project> project;std::unique_ptr<PreviewEngine> live;
    std::atomic<bool> enabled{false},kill{false},rawEnded{false};std::atomic<uint64_t> wanted{0},published{0};
    double rate=44100;int audition=-1;bool stopping=false;juce::String status;
public:
    ~InstrumentPreview(){enabled.store(false);{std::lock_guard<std::mutex> g(requestMutex);stopping=true;}cv.notify_one();if(worker.joinable())worker.join();}
    bool active()const{return enabled.load();}
    void update(const Project& p,double r,int raw=-1){
        std::lock_guard<std::mutex> g(requestMutex);project=p;rate=r;audition=raw;kill.store(false);rawEnded.store(false);++wanted;enabled.store(true);status=p.zones.empty()&&raw<0?"Preview: import samples":"Preview: preparing";
        if(!worker.joinable())worker=std::thread([this]{run();});cv.notify_one();
    }
    void setRate(double r){std::lock_guard<std::mutex> g(requestMutex);if(std::abs(rate-r)>0.01){rate=r;if(enabled.load()&&project){++wanted;status="Preview: preparing";cv.notify_one();}}}
    void disable(){std::lock_guard<std::mutex> g(requestMutex);enabled.store(false);project.reset();++wanted;status=juce::String{};cv.notify_one();}
    void panic(){kill.store(true);}
    juce::String getStatus(){std::lock_guard<std::mutex> g(requestMutex);if(audition>=0&&rawEnded.load()&&published.load()==wanted.load())return "Preview: sample ended";return status;}
    bool render(juce::AudioBuffer<float>& b,juce::MidiBuffer& midi){
        if(!enabled.load())return false;
        std::unique_lock<std::mutex> lock(audioMutex,std::try_to_lock);
        if(lock.owns_lock()&&live&&published.load()==wanted.load()&&b.getNumChannels()>=2){
            if(kill.exchange(false))live->stop();int pos=0;
            for(const auto m:midi){int at=juce::jlimit(pos,b.getNumSamples(),m.samplePosition);live->render(b,pos,at-pos);live->event(m.data,m.numBytes);pos=at;}
            live->render(b,pos,b.getNumSamples()-pos);
            if(live->raw&&!live->rawPlaying)rawEnded.store(true);
        }
        midi.clear();return true;
    }
private:
    void run(){uint64_t seen=0;std::shared_ptr<PreviewCache> cached;for(;;){std::optional<Project> p;double r=0;int raw=-1;uint64_t serial=0;
        {std::unique_lock<std::mutex> g(requestMutex);cv.wait(g,[&]{return stopping||wanted.load()!=seen;});if(stopping)return;
            // Coalesce keyboard autorepeat into one rebuild, with no audio-thread wait.
            while(enabled.load()){auto current=wanted.load();if(!cv.wait_for(g,std::chrono::milliseconds(80),[&]{return stopping||wanted.load()!=current;}))break;if(stopping)return;}
            serial=wanted.load();seen=serial;p=project;r=rate;raw=audition;}
        if(!p)cached.reset();std::unique_ptr<PreviewEngine> next;juce::String result="Preview: import samples";
        try{if(p&&(raw>=0||!p->zones.empty())){next=PreviewEngine::build(std::move(*p),r,raw,cached);result=raw>=0?"Preview: sample audition":"Preview: ready, play MIDI";}}catch(const std::exception& e){result=juce::String("Preview error: ")+juce::String::fromUTF8(e.what());}catch(...){result="Preview error: cannot build instrument";}
        {std::lock_guard<std::mutex> g(requestMutex);if(stopping)return;if(serial!=wanted.load())continue;
            {std::lock_guard<std::mutex> a(audioMutex);live.swap(next);rawEnded.store(false);published.store(serial);}status=enabled.load()?result:juce::String{};}
        // next owns the retired engine; destroy it outside both locks.
    }}
};
}
