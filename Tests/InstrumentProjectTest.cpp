#include "../Source/InstrumentProject.h"
#include <iostream>
using namespace lsfx::instrument;
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    try {
        auto base=std::filesystem::u8path(argv[1]);std::filesystem::create_directories(base);
        Project p;p.name="Editor test";auto a=std::make_shared<Blob>(1024,23);auto b=std::make_shared<Blob>(1024,42);
        int x=p.addSample({"Piano_C3.wav","wav",a,0,48}),y=p.addSample({"Piano_C5.wav","wav",b,0,72});
        int duplicate=p.addSample({"Copy.wav","wav",std::make_shared<Blob>(*a),0,48});require(x==0&&y==1&&duplicate==0&&p.bytes()==2048,"Shared sample deduplication failed");
        p.addZones({y,x},Mapping::multisample);require(p.zones[0].low==0&&p.zones[0].high==60&&p.zones[1].low==61&&p.zones[1].high==127,"Multisample range coverage failed");
        p.addZones({x,y},Mapping::velocity);require(p.zones[2].lowVel==1&&p.zones[2].highVel==63&&p.zones[3].lowVel==64&&p.zones[3].highVel==127,"Velocity range failed");
        p.addZones({x,y},Mapping::chromatic,36);require(p.zones[4].low==36&&p.zones[4].high==36&&p.zones[5].root==37,"Chromatic mapping failed");
        p.addZones({x},Mapping::layer);require(p.zones.back().low==0&&p.zones.back().high==127,"Layer mapping failed");
        require(!p.removeSample(0),"Used sample removed");auto snapshot=p;require(snapshot.samples[0].bytes==p.samples[0].bytes,"Snapshot copied PCM");p.validate(true);
        auto sfz=exportSFZ(p,base/"sfz-test");require(std::filesystem::exists(sfz)&&std::filesystem::file_size(sfz.parent_path()/"Samples"/sampleFilename(0,p.samples[0]))==a->size(),"SFZ package incomplete");
        auto before=std::filesystem::file_size(sfz);bool rejected=false;try{exportSFZ(p,base/"sfz-test");}catch(...){rejected=true;}require(rejected&&std::filesystem::file_size(sfz)==before,"Overwrite protection failed");
        auto sf2=base/"editor.sf2";lsfx::sfzexport::Report report;
        lsfx::sfzexport::Decode decode=[](const Path& path){lsfx::sfzexport::Audio a;a.rate=44100;a.channels=1;for(int i=0;i<8192;++i)a.pcm.push_back(float(.25*std::sin((path.filename()=="sample0001.wav"?.1:.2)*i)));return a;};
        lsfx::sfzexport::convertRegions(regions(p),p.name,sf2,decode,report);
        std::ostringstream memory(std::ios::out|std::ios::binary);lsfx::sfzexport::Report ramReport;lsfx::sfzexport::convertRegionsToStream(regions(p),p.name,memory,decode,ramReport);
        std::ifstream disk(sf2,std::ios::binary);std::string diskBytes((std::istreambuf_iterator<char>(disk)),{});require(memory.str()==diskBytes&&ramReport.regions==report.regions,"Memory SF2 differs from disk export");
        Source result(sf2);require(report.regions==7&&result.presets.count({0,0})==1&&result.ib.size()/4==8,"Direct SF2 region export failed");
        auto bad=p;bad.zones[0].low=127;rejected=false;try{bad.validate();}catch(...){rejected=true;}require(rejected,"Invalid key range accepted");
        std::cout<<"PASS: editor sample sharing, four mapping modes, range validation, non-destructive SFZ package and direct SF2 export\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
