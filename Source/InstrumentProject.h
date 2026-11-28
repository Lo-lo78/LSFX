#pragma once
// Editor-only model. Original audio bytes are immutable and shared by zones,
// snapshots and offline jobs. Nothing here is called from processBlock.
#include "SfzSoundFontExport.h"
#include <memory>
#include <iomanip>
#include <limits>
#include <cstring>
namespace lsfx::instrument {
using namespace sf2export;
constexpr size_t maxSamples=4096, maxZones=4096, maxSampleBytes=256ull*1024*1024, maxProjectBytes=1024ull*1024*1024;
using Blob=std::vector<uint8_t>;
struct Sample {
    std::string name,extension;
    std::shared_ptr<const Blob> bytes;
    uint64_t hash=0;
    int root=60;
    int frames=0; // derived from immutable audio, not separately stored
};
struct Zone {
    int sample=0,root=60,low=0,high=127,lowVel=1,highVel=127,level=0,pan=0,pitch=0; // dB, %, cents
    int startFrame=0,endFrame=-1,loopMode=0,loopStart=0,loopEnd=-1; // inclusive frame indices; -1 = sample end
    int ampDelay=0,ampAttack=0,ampHold=0,ampDecay=0,ampSustain=100,ampRelease=5; // ms, percent
    int cutoff=19912,resonance=0; // Hz, tenths dB
    int modDelay=0,modAttack=0,modHold=0,modDecay=0,modSustain=100,modRelease=0;
    int envPitch=0,envFilter=0; // cents
    int vibDelay=0,vibFreq=5000,vibPitch=0; // ms, mHz, cents
    int lfoDelay=0,lfoFreq=5000,lfoFilter=0,lfoVolume=0; // ms, mHz, cents, tenths dB
    int choke=0,chokeTarget=0,reverb=0,chorus=0,keyTrack=100;
    // SFZ-specific synthesis and region conditions. Kept in projects even in SF2 mode.
    int seqLength=1,seqPosition=1,randomLow=0,randomHigh=1000,trigger=0;
    int offMode=0,offTime=6,filterType=0,ampVelTrack=100,filterVelTrack=0;
    int bendUp=200,bendDown=-200,pitchRandom=0,ampRandom=0,delayRandom=0,offsetRandom=0;
    int stereoWidth=100,stereoPosition=0,filterKeyTrack=0,filterKeyCenter=60;
    int notePolyphony=0,sustainSwitch=1;
    int lfoPitch=0,ampHoldKey=0,ampDecayKey=0,modHoldKey=0,modDecayKey=0;
};
enum class Format {sf2,sfz};
struct ZoneParam { const char* name; const char* key; int Zone::*member; int lo,hi,def; const char* unit; bool sfzOnly=false; bool sf2Only=false; };
inline constexpr ZoneParam zoneParams[]={
 {"Sample","sample",&Zone::sample,0,4095,0,""},
 {"Root Key","root",&Zone::root,0,127,60,""}, {"Low Key","low",&Zone::low,0,127,0,""}, {"High Key","high",&Zone::high,0,127,127,""},
 {"Low Velocity","lowVel",&Zone::lowVel,1,127,1,""}, {"High Velocity","highVel",&Zone::highVel,1,127,127,""}, {"Level","level",&Zone::level,-60,0,0,"dB"}, {"Pan","pan",&Zone::pan,-100,100,0,"percent"},
 {"Pitch","pitch",&Zone::pitch,-4800,4800,0,"cents"}, {"Key Tracking","keyTrack",&Zone::keyTrack,0,1200,100,"percent"},
 {"Sample Start","startFrame",&Zone::startFrame,0,99999999,0,"frames"}, {"Sample End","endFrame",&Zone::endFrame,-1,99999999,-1,"frames"},
 {"Loop Mode","loopMode",&Zone::loopMode,0,3,0,""}, {"Loop Start","loopStart",&Zone::loopStart,0,99999999,0,"frames"}, {"Loop End","loopEnd",&Zone::loopEnd,-1,99999999,-1,"frames"},
 {"Choke Group","choke",&Zone::choke,0,127,0,""},
 {"Amp Delay","ampDelay",&Zone::ampDelay,0,18000,0,"ms"}, {"Amp Attack","ampAttack",&Zone::ampAttack,0,100000,0,"ms"}, {"Amp Hold","ampHold",&Zone::ampHold,0,18000,0,"ms"}, {"Amp Decay","ampDecay",&Zone::ampDecay,0,100000,0,"ms"}, {"Amp Sustain","ampSustain",&Zone::ampSustain,0,100,100,"percent"}, {"Amp Release","ampRelease",&Zone::ampRelease,0,100000,5,"ms"},
 {"Filter Cutoff","cutoff",&Zone::cutoff,20,19912,19912,"Hz"}, {"Filter Resonance","resonance",&Zone::resonance,0,960,0,"0.1 dB"},
 {"Mod Delay","modDelay",&Zone::modDelay,0,18000,0,"ms"}, {"Mod Attack","modAttack",&Zone::modAttack,0,100000,0,"ms"}, {"Mod Hold","modHold",&Zone::modHold,0,18000,0,"ms"}, {"Mod Decay","modDecay",&Zone::modDecay,0,100000,0,"ms"}, {"Mod Sustain","modSustain",&Zone::modSustain,0,100,100,"percent"}, {"Mod Release","modRelease",&Zone::modRelease,0,100000,0,"ms"}, {"Mod Envelope Pitch","envPitch",&Zone::envPitch,-12000,12000,0,"cents"}, {"Mod Envelope Filter","envFilter",&Zone::envFilter,-12000,12000,0,"cents"},
 {"Vibrato Delay","vibDelay",&Zone::vibDelay,0,18000,0,"ms"}, {"Vibrato Frequency","vibFreq",&Zone::vibFreq,1,110000,5000,"mHz"}, {"Vibrato Pitch","vibPitch",&Zone::vibPitch,-12000,12000,0,"cents"},
 {"Mod LFO Delay","lfoDelay",&Zone::lfoDelay,0,18000,0,"ms"}, {"Mod LFO Frequency","lfoFreq",&Zone::lfoFreq,1,110000,5000,"mHz"}, {"Mod LFO Filter","lfoFilter",&Zone::lfoFilter,-12000,12000,0,"cents"}, {"Mod LFO Volume","lfoVolume",&Zone::lfoVolume,-960,960,0,"0.1 dB"}, {"Reverb Send","reverb",&Zone::reverb,0,100,0,"percent"}, {"Chorus Send","chorus",&Zone::chorus,0,100,0,"percent"}, {"Choke Target","chokeTarget",&Zone::chokeTarget,0,127,0,"",true},
 {"Mod LFO Pitch","lfoPitch",&Zone::lfoPitch,-12000,12000,0,"cents"},
 {"Amp Hold Key Scaling","ampHoldKey",&Zone::ampHoldKey,-1200,1200,0,"timecents/key",false,true}, {"Amp Decay Key Scaling","ampDecayKey",&Zone::ampDecayKey,-1200,1200,0,"timecents/key",false,true},
 {"Mod Hold Key Scaling","modHoldKey",&Zone::modHoldKey,-1200,1200,0,"timecents/key",false,true}, {"Mod Decay Key Scaling","modDecayKey",&Zone::modDecayKey,-1200,1200,0,"timecents/key",false,true},
 {"Round Robin Length","seqLength",&Zone::seqLength,1,100,1,"",true}, {"Round Robin Position","seqPosition",&Zone::seqPosition,1,100,1,"",true},
 {"Random Low","randomLow",&Zone::randomLow,0,1000,0,"0.1 percent",true}, {"Random High","randomHigh",&Zone::randomHigh,0,1000,1000,"0.1 percent",true},
 {"Trigger","trigger",&Zone::trigger,0,3,0,"",true}, {"Choke Mode","offMode",&Zone::offMode,0,2,0,"",true}, {"Choke Time","offTime",&Zone::offTime,0,100000,6,"ms",true},
 {"Filter Type","filterType",&Zone::filterType,0,5,0,"",true}, {"Amp Velocity Tracking","ampVelTrack",&Zone::ampVelTrack,-100,100,100,"percent",true},
 {"Filter Velocity Tracking","filterVelTrack",&Zone::filterVelTrack,-9600,9600,0,"cents",true}, {"Bend Up","bendUp",&Zone::bendUp,-9600,9600,200,"cents",true}, {"Bend Down","bendDown",&Zone::bendDown,-9600,9600,-200,"cents",true},
 {"Pitch Random","pitchRandom",&Zone::pitchRandom,0,9600,0,"cents",true}, {"Amp Random","ampRandom",&Zone::ampRandom,0,240,0,"0.1 dB",true},
 {"Delay Random","delayRandom",&Zone::delayRandom,0,100000,0,"ms",true}, {"Offset Random","offsetRandom",&Zone::offsetRandom,0,99999999,0,"frames",true},
 {"Stereo Width","stereoWidth",&Zone::stereoWidth,-100,100,100,"percent",true}, {"Stereo Position","stereoPosition",&Zone::stereoPosition,-100,100,0,"percent",true},
 {"Filter Key Tracking","filterKeyTrack",&Zone::filterKeyTrack,-1200,1200,0,"cents/key",true}, {"Filter Key Center","filterKeyCenter",&Zone::filterKeyCenter,0,127,60,"",true},
 {"Note Polyphony","notePolyphony",&Zone::notePolyphony,0,128,0,"",true}, {"Sustain Switch","sustainSwitch",&Zone::sustainSwitch,0,1,1,"",true}
};
inline int lastFrame(const Sample& s){return (std::max)(0,s.frames-1);}
inline int effectiveEnd(const Zone& z,const Sample& s){return z.endFrame<0?lastFrame(s):z.endFrame;}
inline int effectiveLoopEnd(const Zone& z,const Sample& s){return z.loopEnd<0?effectiveEnd(z,s):z.loopEnd;}
inline void clampFrames(Zone& z,const Sample& s){
    z.startFrame=std::clamp(z.startFrame,0,lastFrame(s));
    if(z.endFrame>=0)z.endFrame=std::clamp(z.endFrame,z.startFrame,lastFrame(s));
    z.offsetRandom=std::clamp(z.offsetRandom,0,effectiveEnd(z,s)-z.startFrame);
    z.loopStart=std::clamp(z.loopStart,z.startFrame,effectiveEnd(z,s));
    if(z.loopEnd>=0)z.loopEnd=std::clamp(z.loopEnd,z.loopStart,effectiveEnd(z,s));
}
inline bool sameSamples(const std::vector<Sample>& a,const std::vector<Sample>& b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].bytes!=b[i].bytes)return false;return true;}
enum class Mapping {layer,chromatic,multisample,velocity};
inline uint64_t fingerprint(const Blob& data) {
    uint64_t h=14695981039346656037ull;
    for(auto c:data) {h^=c;h*=1099511628211ull;}
    return h;
}
inline std::string hashText(uint64_t h) {std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<h;return s.str();}
inline bool extensionOK(const std::string& x) {return x=="wav"||x=="aif"||x=="aiff"||x=="flac";}
inline std::string sampleFilename(size_t i,const Sample& s) {std::ostringstream out;out<<"sample"<<std::setw(4)<<std::setfill('0')<<i+1<<'.'<<s.extension;return out.str();}
struct Project {
    std::string name="New Instrument";
    Format format=Format::sf2;
    std::vector<Sample> samples;
    std::vector<Zone> zones;
    size_t bytes() const {size_t n=0;for(auto& s:samples)if(s.bytes)n+=s.bytes->size();return n;}
    void validate(bool needZones=false) const {
        require(!name.empty()&&name.size()<=256,"Invalid instrument name");
        require(format==Format::sf2||format==Format::sfz,"Invalid instrument format");
        require(samples.size()<=maxSamples&&zones.size()<=maxZones&&bytes()<=maxProjectBytes,"Instrument project size limit exceeded");
        require(!needZones||!zones.empty(),"Import or add a zone first");
        for(const auto& s:samples) require(s.bytes&&!s.bytes->empty()&&s.bytes->size()<=maxSampleBytes&&extensionOK(s.extension)&&!s.name.empty()&&s.name.size()<=1024&&s.root>=0&&s.root<=127,"Invalid sample resource");
        for(const auto& z:zones){
            for(const auto& param:zoneParams)require(z.*param.member>=param.lo&&z.*param.member<=param.hi,"Invalid zone parameter");
            require(z.sample>=0&&size_t(z.sample)<samples.size()&&z.low<=z.high&&z.lowVel<=z.highVel&&z.seqPosition<=z.seqLength&&z.randomLow<=z.randomHigh,"Invalid zone mapping");
            const auto& sample=samples[size_t(z.sample)];if(sample.frames>0){auto fixed=z;clampFrames(fixed,sample);require(fixed.startFrame==z.startFrame&&fixed.endFrame==z.endFrame&&fixed.loopStart==z.loopStart&&fixed.loopEnd==z.loopEnd&&fixed.offsetRandom==z.offsetRandom,"Invalid sample or loop bounds");}
        }
    }
    int addSample(Sample s) {
        require(s.bytes&&!s.bytes->empty()&&s.bytes->size()<=maxSampleBytes&&extensionOK(s.extension),"Invalid sample data");
        s.hash=fingerprint(*s.bytes);
        for(size_t i=0;i<samples.size();++i) if(samples[i].hash==s.hash&&samples[i].bytes->size()==s.bytes->size()&&*samples[i].bytes==*s.bytes) return int(i);
        require(samples.size()<maxSamples&&bytes()<=maxProjectBytes-s.bytes->size(),"Instrument sample memory limit exceeded");
        samples.push_back(std::move(s));return int(samples.size()-1);
    }
    void addZones(const std::vector<int>& ids,Mapping mode,int start=60) {
        require(!ids.empty()&&ids.size()<=maxZones-zones.size(),"Too many zones");
        for(int i:ids)require(i>=0&&size_t(i)<samples.size(),"Invalid sample index");
        require(start>=0&&start<=127,"Invalid mapping start note");
        require(mode!=Mapping::chromatic||ids.size()<=size_t(128-start),"Chromatic mapping exceeds MIDI note 127");
        require(mode!=Mapping::velocity||ids.size()<=127,"Too many velocity layers");
        auto sorted=ids;
        if(mode==Mapping::multisample) {
            std::stable_sort(sorted.begin(),sorted.end(),[&](int a,int b){return samples[size_t(a)].root<samples[size_t(b)].root;});
            // Equal roots are grouped as layers with the same key range.
        }
        for(size_t i=0;i<sorted.size();++i) {
            Zone z;z.sample=sorted[i];z.root=samples[size_t(z.sample)].root;
            if(mode==Mapping::chromatic) z.root=z.low=z.high=start+int(i);
            else if(mode==Mapping::velocity){z.lowVel=1+int(i*127/sorted.size());z.highVel=int((i+1)*127/sorted.size());}
            else if(mode==Mapping::multisample) {
                size_t a=i,b=i;while(a>0&&samples[size_t(sorted[a-1])].root==z.root)--a;while(b+1<sorted.size()&&samples[size_t(sorted[b+1])].root==z.root)++b;
                if(a>0)z.low=(samples[size_t(sorted[a-1])].root+z.root)/2+1;
                if(b+1<sorted.size())z.high=(z.root+samples[size_t(sorted[b+1])].root)/2;
            }
            zones.push_back(z);
        }
    }
    bool removeSample(size_t index) {
        if(index>=samples.size()||std::any_of(zones.begin(),zones.end(),[&](const Zone& z){return size_t(z.sample)==index;}))return false;
        samples.erase(samples.begin()+index);for(auto& z:zones)if(size_t(z.sample)>index)--z.sample;return true;
    }
};
inline bool sameSound(const Project& a,const Project& b){
    if(a.format!=b.format||!sameSamples(a.samples,b.samples)||a.zones.size()!=b.zones.size())return false;
    for(size_t i=0;i<a.zones.size();++i)for(const auto& param:zoneParams)if(a.zones[i].*param.member!=b.zones[i].*param.member)return false;
    return true;
}
inline sfzexport::Ops zoneOps(const Zone& z,const std::string& path,Format fmt=Format::sf2) {
    sfzexport::Ops o={{"sample",path},{"pitch_keycenter",std::to_string(z.root)},{"lokey",std::to_string(z.low)},{"hikey",std::to_string(z.high)},{"lovel",std::to_string(z.lowVel)},{"hivel",std::to_string(z.highVel)},{"volume",std::to_string(z.level)},{"pan",std::to_string(z.pan)},{"tune",std::to_string(z.pitch)}};
    auto set=[&](const char* k,auto v){o[k]=std::to_string(v);};
    set("pitch_keytrack",z.keyTrack);set("offset",z.startFrame);if(z.endFrame>=0)set("end",z.endFrame);
    o["loop_mode"]=z.loopMode==1?"loop_continuous":z.loopMode==2?"loop_sustain":z.loopMode==3&&fmt==Format::sfz?"one_shot":"no_loop";
    if(z.loopMode==1||z.loopMode==2){set("loop_start",z.loopStart);if(z.loopEnd>=0)set("loop_end",z.loopEnd);else if(z.endFrame>=0)set("loop_end",z.endFrame);}
    set("ampeg_delay",z.ampDelay/1000.0);set("ampeg_attack",z.ampAttack/1000.0);set("ampeg_hold",z.ampHold/1000.0);set("ampeg_decay",z.ampDecay/1000.0);set("ampeg_sustain",z.ampSustain);set("ampeg_release",z.ampRelease/1000.0);
    if(fmt==Format::sf2||z.cutoff!=19912||z.resonance||z.envFilter||z.lfoFilter){set("cutoff",z.cutoff);o["fil_type"]="lpf_2p";set("resonance",z.resonance/10.0);}
    auto env=[&](const std::string& prefix,int depth){o[prefix+"depth"]=std::to_string(depth);o[prefix+"delay"]=std::to_string(z.modDelay/1000.0);o[prefix+"attack"]=std::to_string(z.modAttack/1000.0);o[prefix+"hold"]=std::to_string(z.modHold/1000.0);o[prefix+"decay"]=std::to_string(z.modDecay/1000.0);o[prefix+"sustain"]=std::to_string(z.modSustain);o[prefix+"release"]=std::to_string(z.modRelease/1000.0);};
    if(z.envPitch)env("pitcheg_",z.envPitch);if(z.envFilter)env("fileg_",z.envFilter);
    if(z.vibPitch){set("pitchlfo_delay",z.vibDelay/1000.0);set("pitchlfo_freq",z.vibFreq/1000.0);set("pitchlfo_depth",z.vibPitch);}
    if(z.lfoFilter){set("fillfo_delay",z.lfoDelay/1000.0);set("fillfo_freq",z.lfoFreq/1000.0);set("fillfo_depth",z.lfoFilter);}
    if(z.lfoVolume){set("amplfo_delay",z.lfoDelay/1000.0);set("amplfo_freq",z.lfoFreq/1000.0);set("amplfo_depth",z.lfoVolume/10.0);}
    if(z.choke)set("group",z.choke);int target=fmt==Format::sf2?z.choke:z.chokeTarget;if(target)set("off_by",target);
    if(z.reverb)set("effect1",z.reverb);if(z.chorus)set("effect2",z.chorus);
    if(fmt==Format::sf2){
        if(z.lfoPitch){set("lsfx_mod_lfo_pitch",z.lfoPitch);set("lsfx_mod_lfo_freq",z.lfoFreq/1000.0);set("lsfx_mod_lfo_delay",z.lfoDelay/1000.0);}
        if(z.ampHoldKey)set("lsfx_amp_hold_key",z.ampHoldKey);if(z.ampDecayKey)set("lsfx_amp_decay_key",z.ampDecayKey);
        if(z.modHoldKey)set("lsfx_mod_hold_key",z.modHoldKey);if(z.modDecayKey)set("lsfx_mod_decay_key",z.modDecayKey);
    }
    if(fmt==Format::sfz){
        if(z.lfoPitch){set("lfo1_freq",z.lfoFreq/1000.0);set("lfo1_delay",z.lfoDelay/1000.0);set("lfo1_pitch",z.lfoPitch);}
        if(z.seqLength>1){set("seq_length",z.seqLength);set("seq_position",z.seqPosition);}
        if(z.randomLow||z.randomHigh!=1000){set("lorand",z.randomLow/1000.0);set("hirand",z.randomHigh/1000.0);}
        static constexpr const char* triggers[]={"attack","release","first","legato"};
        static constexpr const char* offModes[]={"fast","normal","time"};
        static constexpr const char* filters[]={"lpf_2p","hpf_2p","bpf_2p","brf_2p","lpf_1p","hpf_1p"};
        if(z.trigger)o["trigger"]=triggers[z.trigger];
        if(z.offMode){o["off_mode"]=offModes[z.offMode];if(z.offMode==2)set("off_time",z.offTime/1000.0);}
        if(z.filterType||z.filterVelTrack||z.filterKeyTrack){o["fil_type"]=filters[z.filterType];set("cutoff",z.cutoff);set("resonance",z.resonance/10.0);}
        if(z.ampVelTrack!=100)set("amp_veltrack",z.ampVelTrack);if(z.filterVelTrack)set("fil_veltrack",z.filterVelTrack);
        if(z.bendUp!=200)set("bend_up",z.bendUp);if(z.bendDown!=-200)set("bend_down",z.bendDown);
        if(z.pitchRandom)set("pitch_random",z.pitchRandom);if(z.ampRandom)set("amp_random",z.ampRandom/10.0);
        if(z.delayRandom)set("delay_random",z.delayRandom/1000.0);if(z.offsetRandom)set("offset_random",z.offsetRandom);
        if(z.stereoWidth!=100)set("width",z.stereoWidth);if(z.stereoPosition)set("position",z.stereoPosition);
        if(z.filterKeyTrack){set("fil_keytrack",z.filterKeyTrack);set("fil_keycenter",z.filterKeyCenter);}
        if(z.notePolyphony)set("note_polyphony",z.notePolyphony);if(!z.sustainSwitch)o["sustain_sw"]="off";
    }
    return o;
}
inline std::vector<sfzexport::Region> regions(const Project& p) {
    p.validate(true);std::vector<sfzexport::Region> r;r.reserve(p.zones.size());
    for(const auto& z:p.zones)r.push_back({zoneOps(z,sampleFilename(size_t(z.sample),p.samples[size_t(z.sample)])),{}});
    return r;
}
inline std::string sfzText(const Project& p,const std::vector<std::string>* names=nullptr) {
    p.validate(true);std::ostringstream out;out<<"// Created with LSFX Instrument Editor\n";
    for(const auto& z:p.zones) {auto& sample=p.samples[size_t(z.sample)];auto ops=zoneOps(z,names?names->at(size_t(z.sample)):"Samples/"+sampleFilename(size_t(z.sample),sample),Format::sfz);if((z.loopMode==1||z.loopMode==2)&&sample.frames>0)ops["loop_end"]=std::to_string(effectiveLoopEnd(z,sample));out<<"<region> ";for(const auto& kv:ops)out<<kv.first<<'='<<kv.second<<' ';out<<'\n';}
    return out.str();
}
// sfizz's embedded sample header accepts a base64-encoded audio file. Names
// are private identifiers (not paths); ordinary SFZ export never uses them.
inline std::string base64Audio(const Blob& data) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((data.size()+2)/3*4);
    for(size_t i=0;i<data.size();i+=3){const auto left=data.size()-i;const uint32_t v=(uint32_t(data[i])<<16)|(left>1?uint32_t(data[i+1])<<8:0)|(left>2?uint32_t(data[i+2]):0);
        out+=alphabet[(v>>18)&63];out+=alphabet[(v>>12)&63];out+=left>1?alphabet[(v>>6)&63]:'=';out+=left>2?alphabet[v&63]:'=';}
    return out;
}
inline std::vector<std::string> ramSampleNames(const Project& p){std::vector<std::string> names;names.reserve(p.samples.size());for(size_t i=0;i<p.samples.size();++i)names.push_back("LSFX_"+std::to_string(i)+".wav");return names;}
inline std::set<int> usedSampleIds(const Project& p){std::set<int> ids;for(const auto& z:p.zones)ids.insert(z.sample);return ids;}
// Use a known, uncompressed 32-bit float WAV representation for the memory
// decoder, regardless of original container/codec. The original blob is kept.
inline Blob ramWave(const sfzexport::Audio& a){
    require((a.channels==1||a.channels==2)&&a.rate>0&&!a.pcm.empty()&&a.pcm.size()%size_t(a.channels)==0&&a.pcm.size()<=maxProjectBytes/4,"Invalid or oversized preview PCM");
    Blob b;const auto size=uint32_t(a.pcm.size()*4);b.reserve(size_t(size)+44);
    auto tag=[&](const char* x){b.insert(b.end(),x,x+4);};
    tag("RIFF");add32(b,size+36);tag("WAVE");tag("fmt ");add32(b,16);add16(b,3);add16(b,uint16_t(a.channels));add32(b,uint32_t(a.rate));add32(b,uint32_t(a.rate*a.channels*4));add16(b,uint16_t(a.channels*4));add16(b,32);tag("data");add32(b,size);
    for(float sample:a.pcm){require(std::isfinite(sample),"Invalid sample PCM");uint32_t bits;static_assert(sizeof(bits)==sizeof(sample),"Float32 required");std::memcpy(&bits,&sample,4);add32(b,bits);}return b;
}
inline void writeNew(const Path& path,const void* bytes,size_t size) {
    require(!std::filesystem::exists(path),"Destination already exists; choose a new name");
    std::ofstream out(path,std::ios::binary);require(bool(out),"Cannot create file");out.write(static_cast<const char*>(bytes),std::streamsize(size));out.close();require(bool(out),"Cannot write file");
}
// Destination is a NEW directory; only referenced samples are copied.
inline Path exportSFZ(const Project& p,const Path& directory) {
    p.validate(true);require(std::filesystem::create_directory(directory),"SFZ destination folder already exists; choose a new name");
    try {
        require(std::filesystem::create_directory(directory/"Samples"),"Cannot create Samples folder");
        std::set<int> used;for(const auto& z:p.zones)used.insert(z.sample);
        for(int i:used){const auto& s=p.samples[size_t(i)];writeNew(directory/"Samples"/sampleFilename(size_t(i),s),s.bytes->data(),s.bytes->size());}
        auto dest=directory/std::filesystem::u8path(directory.filename().u8string()+".sfz");const auto text=sfzText(p);writeNew(dest,text.data(),text.size());return dest;
    } catch(...) {std::error_code ec;std::filesystem::remove_all(directory,ec);throw;}
}
}
