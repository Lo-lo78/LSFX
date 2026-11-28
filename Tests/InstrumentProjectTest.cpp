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
        auto advanced=p;advanced.samples[0].frames=8192;advanced.samples[1].frames=8192;auto& z=advanced.zones[0];z.startFrame=64;z.endFrame=4095;z.loopMode=1;z.loopStart=128;z.loopEnd=2047;z.ampAttack=50;z.ampHold=25;z.ampSustain=70;z.cutoff=2400;z.resonance=35;z.envFilter=1200;z.vibPitch=23;z.lfoVolume=15;z.choke=3;z.chokeTarget=4;z.reverb=20;advanced.validate(true);
        auto sf2ops=zoneOps(z,"sample.wav",Format::sf2),sfzops=zoneOps(z,"sample.wav",Format::sfz);require(sf2ops.at("off_by")=="3"&&sfzops.at("off_by")=="4"&&sfzops.at("offset")=="64"&&sfzops.at("loop_end")=="2047","Advanced region opcode values invalid");
        require(std::abs(std::stod(sf2ops.at("ampeg_hold"))-.025)<1e-8&&sf2ops.at("cutoff")=="2400","Envelope/filter parameters not mapped");
        auto broken=z;broken.loopEnd=9000;clampFrames(broken,advanced.samples[size_t(z.sample)]);require(broken.loopEnd==4095,"Loop bounds not clamped");
        auto converted=base/"advanced.sf2";lsfx::sfzexport::Report advancedReport;lsfx::sfzexport::convertRegions(regions(advanced),advanced.name,converted,decode,advancedReport);Source advancedSF2(converted);
        bool exclusive=false,filter=false,hold=false;for(size_t i=0;i<advancedSF2.ig.size();i+=4){auto op=u16(advancedSF2.ig,i),val=u16(advancedSF2.ig,i+2);exclusive|=op==57&&val==3;filter|=op==8&&val==uint16_t(lsfx::sfzexport::clampRound(1200*std::log2(2400.0/8.176),1500,13500));hold|=op==35&&val==uint16_t(lsfx::sfzexport::tc(.025,5000));}require(exclusive&&filter&&hold,"Expanded SF2 generators missing");
        auto changed=advanced;require(sameSound(changed,advanced),"Same project not recognized");changed.name="Renamed";require(sameSound(changed,advanced),"Name caused preview rebuild");changed.zones[0].pan=27;require(!sameSound(changed,advanced),"Changed zone not detected");
        auto sfzProject=advanced;sfzProject.format=Format::sfz;auto& rz=sfzProject.zones[0];rz.seqLength=3;rz.seqPosition=2;rz.randomLow=100;rz.randomHigh=700;rz.trigger=1;rz.filterType=1;rz.offMode=2;rz.offTime=12;rz.pitchRandom=13;rz.notePolyphony=2;rz.sustainSwitch=0;rz.lfoPitch=100;sfzProject.validate(true);
        auto ops=zoneOps(rz,"sample.wav",Format::sfz);require(ops.at("seq_length")=="3"&&ops.at("seq_position")=="2"&&ops.at("trigger")=="release"&&ops.at("fil_type")=="hpf_2p"&&ops.at("off_mode")=="time"&&ops.at("sustain_sw")=="off"&&ops.at("lfo1_pitch")=="100","SFZ articulation mapping");require(!zoneOps(rz,"sample.wav",Format::sf2).count("seq_length"),"SFZ-only fields leaked into SF2");
        auto names=ramSampleNames(sfzProject);auto text=sfzText(sfzProject,&names);require(text.find("sample=LSFX_0.wav")!=std::string::npos&&text.find("Samples/")==std::string::npos,"Memory resource naming failed");require(base64Audio(Blob{'M','a','n'})=="TWFu"&&base64Audio(Blob{'M'})=="TQ=="&&base64Audio(Blob{'M','a'})=="TWE=","Base64 padding failed");
        lsfx::sfzexport::Audio pcm;pcm.rate=48000;pcm.channels=2;pcm.pcm={.25f,-.5f,1.f,0.f};auto wav=ramWave(pcm);require(wav.size()==60&&u16(wav,20)==3&&u16(wav,22)==2&&u32(wav,24)==48000&&u32(wav,40)==16&&u32(wav,44)==0x3e800000&&u32(wav,48)==0xbf000000,"Embedded WAV header or float PCM invalid");
        auto wrong=sfzProject;wrong.zones[0].seqPosition=4;rejected=false;try{wrong.validate();}catch(...){rejected=true;}require(rejected,"Invalid round robin accepted");
        auto more=advanced;more.zones[0].lfoPitch=120;more.zones[0].ampHoldKey=90;more.zones[0].modDecayKey=-100;lsfx::sfzexport::Report extraReport;lsfx::sfzexport::convertRegions(regions(more),more.name,base/"more.sf2",decode,extraReport);Source moreSF2(base/"more.sf2");bool lfo=false,ampKey=false,modKey=false;for(size_t i=0;i<moreSF2.ig.size();i+=4){auto op=u16(moreSF2.ig,i),val=u16(moreSF2.ig,i+2);lfo|=op==5&&val==120;ampKey|=op==39&&val==90;modKey|=op==32&&val==uint16_t(-100);}require(lfo&&ampKey&&modKey,"New SF2 generators missing");
        std::cout<<"PASS: editor sample sharing, four mapping modes, range validation, non-destructive SFZ package and direct SF2 export\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
