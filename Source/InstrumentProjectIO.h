#pragma once
#include <JuceHeader.h>
#include "InstrumentProject.h"
namespace lsfx::instrument {
inline juce::String js(const std::string& s) {return juce::String::fromUTF8(s.c_str());}
inline std::string ss(const juce::String& s) {return s.toStdString();}
inline Path pathOf(const juce::File& f) {return std::filesystem::u8path(f.getFullPathName().toRawUTF8());}
inline juce::File fileOf(const Path& p) {return juce::File(js(p.u8string()));}
inline std::shared_ptr<Blob> readBlob(juce::InputStream& in,juce::int64 size,size_t cap) {
    require(size>0&&uint64_t(size)<=cap,"Invalid or oversized project entry");
    auto b=std::make_shared<Blob>(size_t(size));size_t pos=0;
    while(pos<b->size()) {int n=in.read(b->data()+pos,int(std::min<size_t>(65536,b->size()-pos)));require(n>0,"Truncated sample or project entry");pos+=size_t(n);}
    char extra=0;require(in.read(&extra,1)<=0,"Project entry length mismatch");return b;
}
inline std::unique_ptr<juce::AudioFormatReader> readerFor(juce::AudioFormatManager& formats,const Blob& b) {
    std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(std::make_unique<juce::MemoryInputStream>(b.data(),b.size(),false)));
    require(r&&r->numChannels>=1&&r->numChannels<=2&&r->lengthInSamples>0&&r->lengthInSamples<=100000000&&r->sampleRate>=400&&r->sampleRate<=384000,"Unsupported or invalid mono/stereo audio sample");return r;
}
inline int inferRoot(const juce::AudioFormatReader& reader,const juce::String& name) {
    auto meta=reader.metadataValues.getValue("MidiUnityNote",{});
    if(meta.isNotEmpty()){auto n=meta.getIntValue();if(n>=0&&n<=127)return n;}
    // Recognise an isolated note in a filename: Piano_C4, Bass-F#2, etc.
    static const std::regex notePattern(R"((^|[ _.-])([A-Ga-g][#b]?-?[0-9])($|[ _.-]))");
    std::smatch m;auto text=ss(name);
    if(std::regex_search(text,m,notePattern)){try{auto n=sfzexport::key(m[2].str());if(n>=0&&n<=127)return n;}catch(...) {}}
    return 60;
}
inline Sample importSample(juce::AudioFormatManager& formats,const juce::File& f) {
    auto ext=ss(f.getFileExtension().substring(1).toLowerCase());require(extensionOK(ext),"Only WAV, AIFF and FLAC samples are supported");
    auto stream=f.createInputStream();require(bool(stream),"Cannot open sample");auto b=readBlob(*stream,f.getSize(),maxSampleBytes);
    auto reader=readerFor(formats,*b);return {ss(f.getFileName()),ext,b,fingerprint(*b),inferRoot(*reader,f.getFileNameWithoutExtension()),int(reader->lengthInSamples)};
}
inline sfzexport::Audio decode(juce::AudioFormatManager& formats,const Sample& s) {
    auto r=readerFor(formats,*s.bytes);sfzexport::Audio a;a.rate=int(r->sampleRate);a.channels=int(r->numChannels);a.root=s.root;
    a.pcm.resize(size_t(r->lengthInSamples)*size_t(a.channels));juce::AudioBuffer<float> block(a.channels,32768);
    for(juce::int64 pos=0;pos<r->lengthInSamples;) {int n=int(std::min<juce::int64>(32768,r->lengthInSamples-pos));require(r->read(&block,0,n,pos,true,a.channels==2),"Cannot decode sample");for(int i=0;i<n;++i)for(int ch=0;ch<a.channels;++ch)a.pcm[size_t(pos+i)*a.channels+ch]=block.getSample(ch,i);pos+=n;}
    return a;
}
inline void installNew(const juce::File& temporary,const juce::File& target) {
    require(!target.exists(),"Destination already exists; choose a new name");
    // On Windows rename fails if the destination already exists. On other
    // platforms the explicit existence check is still used.
    std::filesystem::rename(pathOf(temporary),pathOf(target));
}
inline void writeProgramXml(juce::XmlElement& parent,const Project& p,const std::vector<int>& refs){
    parent.setAttribute("name",js(p.name));auto* resources=parent.createNewChildElement("Refs");auto* zones=parent.createNewChildElement("Zones");
    for(size_t i=0;i<p.samples.size();++i){auto* n=resources->createNewChildElement("Ref");n->setAttribute("id",refs.at(i));n->setAttribute("name",js(p.samples[i].name));n->setAttribute("root",p.samples[i].root);}
    for(const auto& z:p.zones){auto* n=zones->createNewChildElement("Zone");for(const auto& param:zoneParams)n->setAttribute(param.key,z.*param.member);for(const auto& m:z.mods){auto* e=n->createNewChildElement("Mod");e->setAttribute("source",m.source);e->setAttribute("dest",m.destination);e->setAttribute("amount",m.amount);e->setAttribute("secondary",m.secondary);e->setAttribute("transform",m.transform);e->setAttribute("sfz",int(m.sfz));}}
}
inline void saveProject(const Project& p,const juce::File& target,bool replaceKnownProject=false,const Library& programs={}) {
    const auto packed=packLibrary(p,programs);const auto& pool=packed.pool;require(target.hasFileExtension("lsfx-instrument"),"Invalid project extension");
    require(!target.exists()||(replaceKnownProject&&target.existsAsFile()),"Project exists; choose a new name");
    juce::XmlElement xml("LSFXInstrument");xml.setAttribute("version",3);xml.setAttribute("name",js(p.name));xml.setAttribute("format",p.format==Format::sf2?"sf2":"sfz");
    auto* samples=xml.createNewChildElement("Samples");
    for(const auto& s:pool.samples){auto* node=samples->createNewChildElement("Sample");node->setAttribute("name",js(s.name));node->setAttribute("extension",js(s.extension));node->setAttribute("root",s.root);node->setAttribute("bytes",juce::String(juce::int64(s.bytes->size())));node->setAttribute("hash",js(hashText(s.hash)));}
    writeProgramXml(*xml.createNewChildElement("Draft"),p,packed.refs[0]);auto* table=xml.createNewChildElement("Programs");size_t i=1;
    for(const auto& item:programs){auto* n=table->createNewChildElement("Program");n->setAttribute("bank",item.first.first);n->setAttribute("program",item.first.second);writeProgramXml(*n,item.second,packed.refs.at(i++));}
    const auto text=xml.toString();require(text.getNumBytesAsUTF8()<=32*1024*1024,"Project manifest too large");juce::ZipFile::Builder zip;
    zip.addEntry(new juce::MemoryInputStream(text.toRawUTF8(),size_t(text.getNumBytesAsUTF8()),true),6,"project.xml",juce::Time::getCurrentTime());
    for(size_t i=0;i<pool.samples.size();++i){const auto& s=pool.samples[i];zip.addEntry(new juce::MemoryInputStream(s.bytes->data(),s.bytes->size(),false),s.extension=="flac"?0:6,"Samples/"+js(sampleFilename(i,s)),juce::Time::getCurrentTime());}
    juce::TemporaryFile temp(target);
    {auto out=temp.getFile().createOutputStream();require(out&&out->openedOk(),"Cannot create project file");require(zip.writeToStream(*out,nullptr),"Cannot compress instrument project");out->flush();require(out->getStatus().wasOk(),"Cannot write instrument project");}
    if(replaceKnownProject) require(temp.overwriteTargetFileWithTemporary(),"Cannot replace project");else installNew(temp.getFile(),target);
}
inline int intAttr(const juce::XmlElement& e,const char* key) {
    require(e.hasAttribute(key),"Missing project attribute");auto s=ss(e.getStringAttribute(key));size_t count=0;int n=std::stoi(s,&count);require(count==s.size(),"Invalid project number");return n;
}
inline std::shared_ptr<Blob> entry(juce::ZipFile& zip,const juce::String& name,size_t cap) {
    const auto* e=zip.getEntry(name);require(e&&!e->isSymbolicLink,"Missing or invalid project entry");std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(*e));require(bool(in),"Cannot open project entry");return readBlob(*in,e->uncompressedSize,cap);
}
inline void readZones(Project& p,const juce::XmlElement* zones){
    require(zones&&zones->getNumChildElements()<=int(maxZones),"Invalid zone table");
    for(auto* n=zones->getFirstChildElement();n;n=n->getNextElement()){require(n->hasTagName("Zone"),"Invalid zone record");Zone z;for(size_t i=0;i<std::size(zoneParams);++i){const auto& param=zoneParams[i];if(n->hasAttribute(param.key))z.*param.member=intAttr(*n,param.key);else if(i==0||i==1||i==2||i==3||i==4||i==5||i==6||i==7||i==8)throw std::runtime_error("Missing core zone parameter");}for(auto* e=n->getFirstChildElement();e;e=e->getNextElement()){require(e->hasTagName("Mod")&&z.mods.size()<48,"Invalid modulator record");int fmt=intAttr(*e,"sfz");require(fmt==0||fmt==1,"Invalid modulation format");z.mods.push_back({intAttr(*e,"source"),intAttr(*e,"dest"),intAttr(*e,"amount"),intAttr(*e,"secondary"),intAttr(*e,"transform"),fmt!=0});}p.zones.push_back(z);}
}
inline Project readProgramXml(const juce::XmlElement& node,const Project& pool){
    Project p;p.format=pool.format;p.name=ss(node.getStringAttribute("name"));auto* refs=node.getChildByName("Refs");require(refs&&refs->getNumChildElements()<=int(maxSamples),"Invalid program sample references");
    for(auto* n=refs->getFirstChildElement();n;n=n->getNextElement()){int id=intAttr(*n,"id");require(n->hasTagName("Ref")&&id>=0&&size_t(id)<pool.samples.size(),"Invalid shared sample reference");auto sample=pool.samples[size_t(id)];sample.name=ss(n->getStringAttribute("name"));sample.root=intAttr(*n,"root");p.samples.push_back(std::move(sample));}
    readZones(p,node.getChildByName("Zones"));p.validate();return p;
}
struct ProjectBundle {Project draft;Library programs;};
inline ProjectBundle openLibraryProject(const juce::File& f) {
    require(f.hasFileExtension("lsfx-instrument")&&f.existsAsFile(),"Invalid instrument project file");juce::ZipFile zip(f);require(zip.getNumEntries()>0&&zip.getNumEntries()<=int(maxSamples+1),"Invalid project archive");
    std::set<std::string> entries;for(int i=0;i<zip.getNumEntries();++i){auto* e=zip.getEntry(i);require(e&&!e->isSymbolicLink&&entries.insert(ss(e->filename)).second,"Duplicate or invalid project archive entry");}
    auto manifest=entry(zip,"project.xml",32*1024*1024);auto xml=juce::parseXML(juce::String::fromUTF8(reinterpret_cast<const char*>(manifest->data()),int(manifest->size())));
    require(xml&&xml->hasTagName("LSFXInstrument")&&(intAttr(*xml,"version")==1||intAttr(*xml,"version")==2||intAttr(*xml,"version")==3),"Unsupported instrument project version");Project p;p.name=ss(xml->getStringAttribute("name"));auto format=xml->getStringAttribute("format");require(format=="sf2"||format=="sfz","Invalid instrument format");p.format=format=="sf2"?Format::sf2:Format::sfz;
    auto* samples=xml->getChildByName("Samples");require(samples&&samples->getNumChildElements()<=int(maxSamples)&&zip.getNumEntries()==samples->getNumChildElements()+1,"Invalid project tables");
    juce::AudioFormatManager formats;formats.registerBasicFormats();size_t total=0;
    for(auto* n=samples->getFirstChildElement();n;n=n->getNextElement()) {
        require(n->hasTagName("Sample"),"Invalid sample record");Sample s;s.name=ss(n->getStringAttribute("name"));s.extension=ss(n->getStringAttribute("extension"));s.root=intAttr(*n,"root");auto size=intAttr(*n,"bytes");require(extensionOK(s.extension)&&size>0&&size_t(size)<=maxSampleBytes&&size_t(size)<=maxProjectBytes-total,"Project sample size limit exceeded");
        s.bytes=entry(zip,"Samples/"+js(sampleFilename(p.samples.size(),s)),size_t(size));require(s.bytes->size()==size_t(size),"Project sample length mismatch");s.hash=fingerprint(*s.bytes);require(js(hashText(s.hash))==n->getStringAttribute("hash"),"Project sample integrity check failed");s.frames=int(readerFor(formats,*s.bytes)->lengthInSamples);total+=s.bytes->size();p.samples.push_back(std::move(s));
    }
    if(intAttr(*xml,"version")<3){readZones(p,xml->getChildByName("Zones"));p.validate();return {std::move(p),{}};}
    auto* work=xml->getChildByName("Draft");auto* table=xml->getChildByName("Programs");require(work&&table&&table->getNumChildElements()<=16512,"Invalid project library");ProjectBundle out;out.draft=readProgramXml(*work,p);size_t zones=0;
    for(auto* n=table->getFirstChildElement();n;n=n->getNextElement()){require(n->hasTagName("Program"),"Invalid program record");int bank=intAttr(*n,"bank"),program=intAttr(*n,"program");require(bank>=0&&bank<=128&&program>=0&&program<=127,"Invalid program address");auto q=readProgramXml(*n,p);require(!q.zones.empty()&&q.zones.size()<=maxZones-zones,"Invalid library zone count");zones+=q.zones.size();require(out.programs.emplace(Address{bank,program},std::move(q)).second,"Duplicate program address");}
    packLibrary(out.draft,out.programs);return out;
}
inline Project openProject(const juce::File& f){return openLibraryProject(f).draft;}
inline juce::File exportInstrument(const Project& p,const juce::File& target) {
    p.validate(true);require(!target.exists(),"Destination exists; choose a new name");
    if(p.format==Format::sfz){require(target.hasFileExtension("sfz"),"Invalid SFZ destination");return fileOf(exportSFZ(p,pathOf(target.getParentDirectory().getChildFile(target.getFileNameWithoutExtension()))));}
    require(target.hasFileExtension("sf2"),"Invalid SF2 destination");juce::TemporaryFile temp(target);
    juce::AudioFormatManager formats;formats.registerBasicFormats();std::map<Path,size_t> indices;for(size_t i=0;i<p.samples.size();++i)indices.emplace(std::filesystem::u8path(sampleFilename(i,p.samples[i])),i);
    sfzexport::Report report;sfzexport::convertRegions(regions(p),p.name,pathOf(temp.getFile()),[&](const Path& key){auto it=indices.find(key);require(it!=indices.end(),"Invalid editor sample reference");return decode(formats,p.samples[it->second]);},report);
    require(report.regions==p.zones.size()&&report.skipped==0,"Instrument export lost zones");
    // Read the generated tables before installing the file.
    {sf2export::Source verified(pathOf(temp.getFile()));require(verified.presets.count({0,0})==1,"Generated SoundFont missing program 0");}
    installNew(temp.getFile(),target);return target;
}
inline juce::File exportLibrary(const Project& work,const Library& library,const juce::File& target){
    require(!library.empty(),"Save at least one program in F6 before exporting");require(!target.exists(),"Destination exists; choose a new name");
    if(work.format==Format::sfz){require(target.hasFileExtension("sfz"),"Invalid SFZ destination");return fileOf(exportSFZLibrary(work,library,pathOf(target.getParentDirectory().getChildFile(target.getFileNameWithoutExtension()))));}
    require(target.hasFileExtension("sf2"),"Invalid SF2 destination");juce::TemporaryFile temp(target);juce::AudioFormatManager formats;formats.registerBasicFormats();
    {std::fstream out(pathOf(temp.getFile()),std::ios::binary|std::ios::out|std::ios::trunc);require(bool(out),"Cannot open export file");Project label;label.format=work.format;label.name=ss(target.getFileNameWithoutExtension());if(label.name.empty()||label.name.size()>256)label.name="LSFX library";writeSF2Library(label,library,out,[&](const Sample& sample){return decode(formats,sample);});}
    {sf2export::Source verified(pathOf(temp.getFile()));require(verified.presets.size()==library.size(),"Export lost programs");for(const auto& item:library)require(verified.presets.count(item.first)==1,"Export lost a program address");}
    installNew(temp.getFile(),target);return target;
}
struct EditorPosition {int page=0,sample=0,zone=0,parameter=0,mode=0,start=60,step=0,focus=10,modSource=129,modRow=0,modColumn=0,bank=0,program=0;};
struct Draft {Project project;Library programs;juce::File savedFile;bool dirty=false;EditorPosition position;std::vector<Project> undo,redo;std::optional<Zone> clipboard;};
}
