#pragma once
// Editor-only model. Original audio bytes are immutable and shared by zones,
// snapshots and offline jobs. Nothing here is called from processBlock.
#include "SfzSoundFontExport.h"
#include <memory>
#include <iomanip>
#include <limits>
namespace lsfx::instrument {
using namespace sf2export;
constexpr size_t maxSamples=4096, maxZones=4096, maxSampleBytes=256ull*1024*1024, maxProjectBytes=1024ull*1024*1024;
using Blob=std::vector<uint8_t>;
struct Sample {
    std::string name,extension;
    std::shared_ptr<const Blob> bytes;
    uint64_t hash=0;
    int root=60;
};
struct Zone {
    int sample=0,root=60,low=0,high=127,lowVel=1,highVel=127,level=0,pan=0,pitch=0; // dB, %, cents
};
enum class Format {sf2,sfz};
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
        for(const auto& z:zones) require(z.sample>=0&&size_t(z.sample)<samples.size()&&z.root>=0&&z.root<=127&&z.low>=0&&z.high<=127&&z.low<=z.high&&z.lowVel>=1&&z.highVel<=127&&z.lowVel<=z.highVel&&z.level>=-60&&z.level<=0&&z.pan>=-100&&z.pan<=100&&z.pitch>=-4800&&z.pitch<=4800,"Invalid zone parameters");
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
inline sfzexport::Ops zoneOps(const Zone& z,const std::string& path) {
    return {{"sample",path},{"pitch_keycenter",std::to_string(z.root)},{"lokey",std::to_string(z.low)},{"hikey",std::to_string(z.high)},{"lovel",std::to_string(z.lowVel)},{"hivel",std::to_string(z.highVel)},{"volume",std::to_string(z.level)},{"pan",std::to_string(z.pan)},{"tune",std::to_string(z.pitch)},{"loop_mode","no_loop"},{"ampeg_release","0.005"}};
}
inline std::vector<sfzexport::Region> regions(const Project& p) {
    p.validate(true);std::vector<sfzexport::Region> r;r.reserve(p.zones.size());
    for(const auto& z:p.zones)r.push_back({zoneOps(z,sampleFilename(size_t(z.sample),p.samples[size_t(z.sample)])),{}});
    return r;
}
inline std::string sfzText(const Project& p) {
    p.validate(true);std::ostringstream out;out<<"// Created with LSFX Instrument Editor\n";
    for(const auto& z:p.zones) {out<<"<region> ";for(const auto& kv:zoneOps(z,"Samples/"+sampleFilename(size_t(z.sample),p.samples[size_t(z.sample)])))out<<kv.first<<'='<<kv.second<<' ';out<<'\n';}
    return out.str();
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
