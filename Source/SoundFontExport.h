#pragma once
// Selective, lossless PCM16 SF2 assembly. No engine or plug-in state is exported.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
namespace lsfx::sf2export {
using Bytes=std::vector<uint8_t>;
using Path=std::filesystem::path;
using U64=uint64_t;
inline void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
inline uint16_t u16(const Bytes& b,size_t p) {require(p<=b.size()&&b.size()-p>=2,"Invalid SF2 index");return uint16_t(b[p]|(uint16_t(b[p+1])<<8));}
inline uint32_t u32(const Bytes& b,size_t p) {return uint32_t(u16(b,p))|(uint32_t(u16(b,p+2))<<16);}
inline void put16(Bytes& b,size_t p,uint16_t v) {b.at(p)=uint8_t(v);b.at(p+1)=uint8_t(v>>8);}
inline void put32(Bytes& b,size_t p,uint32_t v) {put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
inline void add16(Bytes& b,uint16_t v) {b.push_back(uint8_t(v));b.push_back(uint8_t(v>>8));}
inline void add32(Bytes& b,uint32_t v) {add16(b,uint16_t(v));add16(b,uint16_t(v>>16));}
inline void append(Bytes& to,const Bytes& from) {to.insert(to.end(),from.begin(),from.end());}
inline Bytes slice(const Bytes& b,size_t p,size_t n) {require(p<=b.size()&&n<=b.size()-p,"Truncated SF2 record");return Bytes(b.begin()+p,b.begin()+p+n);}
inline void tag(Bytes& b,const char* s) {b.insert(b.end(),s,s+4);}
inline Bytes record(size_t n,const char* name) {Bytes b(n);for(size_t i=0;i<20&&name[i];++i)b[i]=uint8_t(name[i]);return b;}
inline uint32_t size32(U64 n) {require(n<=0xffffffffULL,"SF2 RIFF size limit exceeded");return uint32_t(n);}
inline uint16_t index16(size_t n) {require(n<=65535,"SF2 16-bit table index limit exceeded");return uint16_t(n);}
inline void checkCancel(const std::atomic<bool>* c) {if(c&&c->load())throw std::runtime_error("Export cancelled");}
struct Entry { Path path; int sourceBank,sourceProgram,bank,program; };
struct Result {size_t presets=0,instruments=0,samples=0,sources=0;U64 bytes=0;};
struct Chunk {U64 pos=0,size=0;};
struct Source {
    Path path; std::ifstream file; U64 fileSize=0; Chunk pcm;
    Bytes ph,pb,pm,pg,ih,ib,im,ig,sh;
    std::string name,author,copyright;
    std::map<std::pair<int,int>,size_t> presets;
    std::map<size_t,uint16_t> instruments,samples;
    explicit Source(Path p):path(std::move(p)),file(path,std::ios::binary) {
        require(bool(file),"Cannot open source SF2");file.seekg(0,std::ios::end);auto end=file.tellg();require(end>=12,"Invalid RIFF file");fileSize=U64(end);
        auto h=read(0,12);require(str(h,0)=="RIFF"&&str(h,8)=="sfbk","Not a standard SF2 RIFF file");
        U64 limit=8+U64(u32(h,4));require(limit<=fileSize&&limit>=12,"Truncated RIFF file");
        std::map<std::string,Chunk> tables;bool version=false;
        walk(12,limit,[&](const std::string& id,Chunk c){if(id!="LIST")return;require(c.size>=4,"Invalid RIFF LIST");auto kind=str(read(c.pos,4),0);
            walk(c.pos+4,c.pos+c.size,[&](const std::string& name,Chunk a){
                if(kind=="INFO"&&name=="ifil") {require(a.size==4,"Invalid SF2 version chunk");auto v=read(a.pos,4);require(u16(v,0)==2,"Only SoundFont 2 sources are supported");version=true;}
                if(kind=="INFO"&&(name=="INAM"||name=="IENG"||name=="ICOP")) {auto bytes=read(a.pos,size_t(std::min<U64>(a.size,256)));std::string value;for(auto ch:bytes){if(!ch)break;value+=ch>=32&&ch<127?char(ch):' ';}if(name=="INAM")this->name=value;else if(name=="IENG")author=value;else copyright=value;}
                if(kind=="sdta"&&name=="smpl") {require(!pcm.pos,"Duplicate sample chunk");pcm=a;}
                if(kind=="sdta"&&name=="sm24") throw std::runtime_error("24-bit SF2 source not yet supported; export stopped without reducing quality");
                if(kind=="pdta")require(tables.emplace(name,a).second,"Duplicate SF2 table");
            });
        });
        require(version&&pcm.pos&&pcm.size%2==0,"Invalid SF2 sample data");
        auto load=[&](const char* n,size_t width){auto i=tables.find(n);require(i!=tables.end(),"Missing SF2 table");auto a=i->second;require(a.size>=width&&a.size%width==0&&a.size/width<=65536,"Invalid SF2 table size");return read(a.pos,size_t(a.size));};
        ph=load("phdr",38);pb=load("pbag",4);pm=load("pmod",10);pg=load("pgen",4);ih=load("inst",22);ib=load("ibag",4);im=load("imod",10);ig=load("igen",4);sh=load("shdr",46);
        for(size_t i=0;i+1<ph.size()/38;++i)require(presets.emplace(std::make_pair(u16(ph,i*38+22),u16(ph,i*38+20)),i).second,"Duplicate source preset address");
    }
    Bytes read(U64 pos,size_t n) {require(pos<=fileSize&&U64(n)<=fileSize-pos,"Truncated SF2 chunk");Bytes b(n);file.clear();file.seekg(std::streamoff(pos));if(n)file.read(reinterpret_cast<char*>(b.data()),std::streamsize(n));require(bool(file),"Cannot read source SF2");return b;}
    static std::string str(const Bytes& b,size_t p) {require(p+4<=b.size(),"Invalid RIFF tag");return std::string(b.begin()+p,b.begin()+p+4);}
    template<class F> void walk(U64 pos,U64 end,F f) {while(pos<end) {require(end-pos>=8,"Truncated RIFF chunk header");auto h=read(pos,8);U64 n=u32(h,4);require(n+(n&1)<=end-pos-8,"Invalid RIFF chunk length");f(str(h,0),Chunk{pos+8,n});pos+=8+n+(n&1);}}
};
struct SampleCopy {Source* source;U64 start,count;};
class Builder {
    std::map<Path,std::unique_ptr<Source>> sources;
    Bytes ph,pb,pm,pg,ih,ib,im,ig,sh;
    std::vector<SampleCopy> copies;
    U64 sampleCount=0;
    const std::atomic<bool>* cancel;
    std::atomic<int>* progress;
    Source& get(const Path& p) {auto it=sources.find(p);if(it==sources.end())it=sources.emplace(p,std::make_unique<Source>(p)).first;return *it->second;}
    // Address offsets can refer beyond a sample's nominal start/end. Reject
    // such selected zones rather than silently changing their sound or copying
    // unrelated sample data. Positive start and negative end offsets are kept.
    using OffsetGenerators=std::array<int64_t,8>;
    void readOffsets(const Bytes& gens,OffsetGenerators& v) {
        constexpr std::array<uint16_t,8> ops{0,1,2,3,4,12,45,50};
        for(size_t i=0;i<gens.size();i+=4) {auto op=u16(gens,i);auto raw=u16(gens,i+2);for(size_t k=0;k<ops.size();++k)if(op==ops[k])v[k]=raw<32768?raw:int64_t(raw)-65536;}
    }
    uint16_t sample(Source& s,size_t id) {
        if(auto it=s.samples.find(id);it!=s.samples.end())return it->second;
        checkCancel(cancel);require(id+1<s.sh.size()/46,"Invalid sample reference");auto r=slice(s.sh,id*46,46);auto type=u16(r,44);require(type==1||type==2||type==4||type==8,"Unsupported ROM or compressed sample");
        U64 a=u32(r,20),b=u32(r,24),l=u32(r,28),e=u32(r,32);require(a<b&&b<=s.pcm.size/2,"Invalid sample bounds");
        // A loop may be unused and have zero/invalid coordinates in old banks.
        // Preserve valid relative loop points; use safe endpoints for invalid ones.
        if(l<a||l>b||e<l||e>b){l=a;e=b;}
        auto out=index16(sh.size()/46);s.samples.emplace(id,out);auto pos=sh.size();
        require(sampleCount+(b-a)+46<=0x7fffffffULL,"SF2 sample data exceeds RIFF limit");
        put32(r,20,size32(sampleCount));put32(r,24,size32(sampleCount+b-a));put32(r,28,size32(sampleCount+l-a));put32(r,32,size32(sampleCount+e-a));
        sampleCount+=b-a+46;copies.push_back({&s,a,b-a});append(sh,r);
        if(type!=1) {auto link=u16(r,42);require(link!=id&&size_t(link)+1<s.sh.size()/46,"Invalid stereo sample link");put16(sh,pos+42,sample(s,link));} else put16(sh,pos+42,0);
        return out;
    }
    template<class F> void zones(const Bytes& bags,const Bytes& gens,const Bytes& mods,size_t start,size_t end,Bytes& db,Bytes& dg,Bytes& dm,F transform) {
        require(start<=end&&end<bags.size()/4,"Invalid SF2 zone range");
        for(size_t z=start;z<end;++z) {
            checkCancel(cancel);size_t ga=u16(bags,z*4),gb=u16(bags,(z+1)*4),ma=u16(bags,z*4+2),mb=u16(bags,(z+1)*4+2);
            require(ga<=gb&&gb<=gens.size()/4&&ma<=mb&&mb<=mods.size()/10,"Invalid SF2 generator or modulator range");
            add16(db,index16(dg.size()/4));add16(db,index16(dm.size()/10));
            auto g=slice(gens,ga*4,(gb-ga)*4);transform(g,z==start);append(dg,g);append(dm,slice(mods,ma*10,(mb-ma)*10));
            index16(db.size()/4);index16(dg.size()/4);index16(dm.size()/10);
        }
    }
    uint16_t instrument(Source& s,size_t id) {
        if(auto it=s.instruments.find(id);it!=s.instruments.end())return it->second;
        require(id+1<s.ih.size()/22,"Invalid instrument reference");auto out=index16(ih.size()/22);s.instruments.emplace(id,out);
        auto r=slice(s.ih,id*22,22);auto a=u16(r,20),b=u16(s.ih,(id+1)*22+20);put16(r,20,index16(ib.size()/4));append(ih,r);
        OffsetGenerators global{};int globalMode=0;
        zones(s.ib,s.ig,s.im,a,b,ib,ig,im,[&](Bytes& g,bool first){
            bool local=false;for(size_t k=0;k<g.size();k+=4)if(u16(g,k)==53) {
                auto sid=u16(g,k+2);require(size_t(sid)+1<s.sh.size()/46,"Invalid instrument sample");auto values=global;readOffsets(g,values);std::array<int64_t,4> off{};for(size_t j=0;j<4;++j)off[j]=values[j]+32768*values[j+4];
                int mode=globalMode;for(size_t j=0;j<g.size();j+=4)if(u16(g,j)==54)mode=u16(g,j+2);
                auto start=int64_t(u32(s.sh,sid*46+20)),end=int64_t(u32(s.sh,sid*46+24));auto l=int64_t(u32(s.sh,sid*46+28)),e=int64_t(u32(s.sh,sid*46+32));
                require(off[0]>=0&&off[1]<=0&&start+off[0]<end+off[1],"Selected SF2 uses sample address offsets outside its sample; cannot export losslessly yet");
                if((mode&1)||off[2]||off[3])require(l>=start&&e<=end&&l<e&&l+off[2]>=start&&e+off[3]<=end&&l+off[2]<e+off[3],"Selected SF2 uses external loop offsets; cannot export losslessly yet");
                put16(g,k+2,sample(s,sid));local=true;
            }if(first&&!local){readOffsets(g,global);for(size_t j=0;j<g.size();j+=4)if(u16(g,j)==54)globalMode=u16(g,j+2);}
        });return out;
    }
    void preset(const Entry& e) {
        auto& s=get(e.path);auto it=s.presets.find({e.sourceBank,e.sourceProgram});require(it!=s.presets.end(),"Source preset no longer exists");size_t id=it->second;auto r=slice(s.ph,id*38,38);auto a=u16(r,24),b=u16(s.ph,(id+1)*38+24);
        put16(r,20,uint16_t(e.program));put16(r,22,uint16_t(e.bank));put16(r,24,index16(pb.size()/4));append(ph,r);
        zones(s.pb,s.pg,s.pm,a,b,pb,pg,pm,[&](Bytes& g,bool){for(size_t k=0;k<g.size();k+=4)if(u16(g,k)==41)put16(g,k+2,instrument(s,u16(g,k+2)));});
    }
    static Bytes chunk(const char* id,const Bytes& data) {Bytes b;tag(b,id);add32(b,size32(data.size()));append(b,data);if(data.size()&1)b.push_back(0);return b;}
    static Bytes text(const std::string& s) {Bytes b(s.begin(),s.end());b.push_back(0);if(b.size()&1)b.push_back(0);return b;}
    static void write(std::ostream& f,const Bytes& b) {if(!b.empty())f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));require(bool(f),"Cannot write SF2 (check free disk space)");}
public:
    Builder(const std::atomic<bool>* c,std::atomic<int>* p):cancel(c),progress(p){}
    Result run(std::vector<Entry> entries,const Path& output) {
        require(!entries.empty(),"The map is empty");std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){return std::tie(a.bank,a.program)<std::tie(b.bank,b.program);});
        std::set<std::pair<int,int>> addresses;
        for(size_t i=0;i<entries.size();++i) {checkCancel(cancel);auto& e=entries[i];require(e.bank>=0&&e.bank<=128&&e.program>=0&&e.program<=127,"Bank outside standard SF2 range 0-128");require(addresses.emplace(e.bank,e.program).second,"Duplicate destination address");preset(e);if(progress)progress->store(int(30*(i+1)/entries.size()));}
        index16(ih.size()/22);index16(sh.size()/46);
        Result result{entries.size(),ih.size()/22,sh.size()/46,sources.size(),0};
        auto p=record(38,"EOP");put16(p,24,index16(pb.size()/4));append(ph,p);auto i=record(22,"EOI");put16(i,20,index16(ib.size()/4));append(ih,i);append(sh,record(46,"EOS"));
        add16(pb,index16(pg.size()/4));add16(pb,index16(pm.size()/10));add16(ib,index16(ig.size()/4));add16(ib,index16(im.size()/10));pg.resize(pg.size()+4);ig.resize(ig.size()+4);pm.resize(pm.size()+10);im.resize(im.size()+10);
        Bytes info;tag(info,"INFO");Bytes version;add16(version,2);add16(version,1);append(info,chunk("ifil",version));append(info,chunk("isng",text("EMU8000")));append(info,chunk("INAM",text("LSFX exported SoundFont")));append(info,chunk("ISFT",text("LSFX")));std::string comment="Assembled from selected user sources. Original source licenses apply.";
        for(auto& item:sources) {auto& src=*item.second;auto line="\n"+src.name+"; "+src.author+"; "+src.copyright;if(comment.size()+line.size()<65000)comment+=line;}
        append(info,chunk("ICMT",text(comment)));info=chunk("LIST",info);
        Bytes pd;tag(pd,"pdta");append(pd,chunk("phdr",ph));append(pd,chunk("pbag",pb));append(pd,chunk("pmod",pm));append(pd,chunk("pgen",pg));append(pd,chunk("inst",ih));append(pd,chunk("ibag",ib));append(pd,chunk("imod",im));append(pd,chunk("igen",ig));append(pd,chunk("shdr",sh));pd=chunk("LIST",pd);
        U64 pcmBytes=sampleCount*2,sdtaBytes=12+pcmBytes;Bytes header;tag(header,"RIFF");add32(header,size32(4+info.size()+8+sdtaBytes+pd.size()));tag(header,"sfbk");append(header,info);tag(header,"LIST");add32(header,size32(sdtaBytes));tag(header,"sdta");tag(header,"smpl");add32(header,size32(pcmBytes));
        // Caller supplies a temporary file; never truncate a source file.
        for(auto& e:entries) {std::error_code ec;require(!std::filesystem::equivalent(output,e.path,ec),"Cannot overwrite a source SF2");}
        std::ofstream f(output,std::ios::binary|std::ios::trunc);require(bool(f),"Cannot create temporary SF2");write(f,header);U64 done=0;Bytes guard(92);
        for(auto& c:copies){U64 offset=0;while(offset<c.count){checkCancel(cancel);auto n=std::min<U64>(32768,c.count-offset);write(f,c.source->read(c.source->pcm.pos+2*(c.start+offset),size_t(2*n)));offset+=n;done+=n;if(progress)progress->store(30+int(69*done/sampleCount));}write(f,guard);done+=46;}
        checkCancel(cancel);write(f,pd);f.flush();require(bool(f),"Cannot flush SF2");f.close();require(bool(f),"Cannot close SF2");result.bytes=header.size()+pcmBytes+pd.size();if(progress)progress->store(100);return result;
    }
};
inline Result exportFile(std::vector<Entry> entries,const Path& temporary,const std::atomic<bool>* cancel=nullptr,std::atomic<int>* progress=nullptr) {return Builder(cancel,progress).run(std::move(entries),temporary);}
}
