#include "../Source/InstrumentSourceImport.h"
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
        // A reload cannot assume the engine still owns any embedded PCM.
        // Each document must contain the full referenced asset set, deduplicated.
        auto reload=sfzProject;int decodes=0;
        auto decodeRam=[&](const Sample&){++decodes;return pcm;};
        auto countHeader=[](const std::string& doc,const std::string& header){size_t count=0,pos=0;while((pos=doc.find(header,pos))!=std::string::npos){++count;pos+=header.size();}return count;};
        for(int edit=0;edit<4;++edit){
            reload.zones[0].pan=edit*10;decodes=0;
            const auto doc=sfzMemoryText(reload,decodeRam);
            require(decodes==2&&countHeader(doc,"<sample> name=LSFX_0.wav base64data=")==1&&countHeader(doc,"<sample> name=LSFX_1.wav base64data=")==1,"Reload document omitted or duplicated resident audio");
            require(doc.find("<sample>")<doc.find("<region>")&&doc.find("hint_ram_based")==std::string::npos,"Invalid embedded SFZ document layout");
        }
        auto oneAsset=reload;for(auto& zone:oneAsset.zones)zone.sample=0;decodes=0;
        const auto singleDoc=sfzMemoryText(oneAsset,decodeRam);require(decodes==1&&singleDoc.find("<sample> name=LSFX_1.wav")==std::string::npos,"Unused asset embedded");
        decodes=0;require(sfzMemoryText(reload,decodeRam).find("<sample> name=LSFX_1.wav")!=std::string::npos&&decodes==2,"Reintroduced asset omitted");
        auto wrong=sfzProject;wrong.zones[0].seqPosition=4;rejected=false;try{wrong.validate();}catch(...){rejected=true;}require(rejected,"Invalid round robin accepted");
        auto more=advanced;more.zones[0].lfoPitch=120;more.zones[0].ampHoldKey=90;more.zones[0].modDecayKey=-100;lsfx::sfzexport::Report extraReport;lsfx::sfzexport::convertRegions(regions(more),more.name,base/"more.sf2",decode,extraReport);Source moreSF2(base/"more.sf2");bool lfo=false,ampKey=false,modKey=false;for(size_t i=0;i<moreSF2.ig.size();i+=4){auto op=u16(moreSF2.ig,i),val=u16(moreSF2.ig,i+2);lfo|=op==5&&val==120;ampKey|=op==39&&val==90;modKey|=op==32&&val==uint16_t(-100);}require(lfo&&ampKey&&modKey,"New SF2 generators missing");
        auto modded=advanced;auto& mzone=modded.zones[0];mzone.mods={{128+74,8,2400,0,0,false},{2+256+1024,48,600,0,0,false},{128+1,17,500,0,0,true}};
        modded.validate(true);require(!sameSound(modded,advanced),"Modulation edit ignored by preview equality");
        auto moddedRegions=regions(modded);require(moddedRegions[0].nativeMods.size()==2,"SF2 modulator export lost or mixed format");
        lsfx::sfzexport::Report modReport;auto modPath=base/"modulated.sf2";lsfx::sfzexport::convertRegions(moddedRegions,modded.name,modPath,decode,modReport);Source modSF2(modPath);bool cutoffMod=false,velocityMod=false;
        for(size_t i=0;i<modSF2.im.size();i+=10){auto src=u16(modSF2.im,i),dst=u16(modSF2.im,i+2),amount=u16(modSF2.im,i+4);cutoffMod|=src==202&&dst==8&&amount==2400;velocityMod|=src==1282&&dst==48&&amount==600;}require(cutoffMod&&velocityMod,"Native SF2 modulator bytes missing");
        modded.format=Format::sfz;auto modOps=zoneOps(mzone,"sample.wav",Format::sfz);require(std::abs(std::stod(modOps.at("pan_oncc1"))-50)<.001&&!modOps.count("cutoff_oncc74"),"SFZ CC mapping mixed formats");
        auto dup=modded;dup.zones[0].mods.push_back(dup.zones[0].mods[0]);rejected=false;try{dup.validate();}catch(...){rejected=true;}require(rejected,"Duplicate modulator accepted");
        auto invalid=modded;invalid.zones[0].mods[2].amount=1001;rejected=false;try{invalid.validate();}catch(...){rejected=true;}require(rejected,"Invalid SFZ amount accepted");
        // A library contains multiple MIDI addresses, with one shared pool.
        Project first;first.name="First";first.samples={p.samples[0]};first.addZones({0},Mapping::chromatic,60);
        Project second=first;second.name="Second";second.samples[0].bytes=std::make_shared<Blob>(*first.samples[0].bytes);second.zones[0].low=second.zones[0].high=62;
        Library lib;storeProgram(lib,first,{2,4},false);storeProgram(lib,second,{128,0},false);
        require(lib.size()==2&&lib.at({2,4}).samples[0].bytes==lib.at({128,0}).samples[0].bytes,"Program audio not shared");
        require(packLibrary(second,lib).pool.samples.size()==1,"Duplicate archive sample");
        second.name="Uncommitted";second.zones[0].pan=99;require(lib.at({128,0}).name=="Second"&&lib.at({128,0}).zones[0].pan==0,"Working draft changed saved program");
        rejected=false;try{storeProgram(lib,second,{2,4},false);}catch(...){rejected=true;}require(rejected&&lib.at({2,4}).name=="First","Unconfirmed overwrite accepted");
        auto libPath=base/"library.sf2";{std::ofstream f(libPath,std::ios::binary);writeSF2Library(second,lib,f,[&](const Sample&){return decode(Path("sample0001.wav"));});}
        Source libSF2(libPath);require(libSF2.presets.size()==2&&libSF2.presets.count({2,4})&&libSF2.presets.count({128,0}),"Library SF2 lost bank/program addresses");require(libSF2.sh.size()/46==2,"Library SF2 duplicated sample");require(libSF2.ih.size()/22==3&&u16(libSF2.ih,42)==1&&u16(libSF2.ih,64)==2,"Library instrument boundaries invalid");
        require(std::string(libSF2.ph.begin(),libSF2.ph.begin()+5)=="First"&&std::string(libSF2.ph.begin()+38,libSF2.ph.begin()+44)=="Second","Export used draft name instead of saved program");
        auto zlib=lib;for(auto& item:zlib)item.second.format=Format::sfz;second.format=Format::sfz;auto zpath=exportSFZLibrary(second,zlib,base/"library-sfz");require(std::filesystem::exists(zpath/"Bank002_Program004.sfz")&&std::filesystem::exists(zpath/"Bank128_Program000.sfz")&&std::filesystem::exists(zpath/"Programs.csv"),"SFZ library files missing");require(std::distance(std::filesystem::directory_iterator(zpath/"Samples"),std::filesystem::directory_iterator{})==1,"SFZ library duplicated samples");
        rejected=false;try{exportSFZLibrary(second,zlib,zpath);}catch(...){rejected=true;}require(rejected,"Library export overwrite accepted");
        // A selected source address becomes an independent, editable draft.
        require(legacyLevel(-20,Format::sf2)==-800&&legacyLevel(-20,Format::sfz)==-2000,"Legacy level migration failed");
        auto exact=lib.at({2,4});exact.zones[0].level=-1236;exact.zones[0].nativeGenerators={{17,13,0},{34,-8123,0},{37,17,100}};auto exactPath=base/"exact.sf2";lsfx::sfzexport::Report exactReport;lsfx::sfzexport::convertRegions(regions(exact),exact.name,exactPath,decode,exactReport);
        auto exactImport=importSF2Program(exactPath,0,0).project;auto& ez=exactImport.zones[0];require(ez.level==-1236&&sf2FileAttenuation(ez.level)==309&&std::abs(sf2VoiceAttenuation(ez.level)-123.6)<1e-8,"SF2 attenuation changed on import");
        require(activeNativeGenerators(ez).at(17)==13&&activeNativeGenerators(ez).at(34)==-8123&&activeNativeGenerators(ez).at(37)==17,"Native generator precision lost");
        auto exactRound=base/"exact-round.sf2";lsfx::sfzexport::Report roundReport;lsfx::sfzexport::convertRegions(regions(exactImport),exactImport.name,exactRound,decode,roundReport);auto roundImport=importSF2Program(exactRound,0,0).project;require(roundImport.zones[0].level==ez.level&&activeNativeGenerators(roundImport.zones[0])==activeNativeGenerators(ez),"SF2 exact generator round trip failed");
        ez.pan=20;require(!activeNativeGenerators(ez).count(17),"Changed UI parameter retained stale native value");
        SourceImport modImport;std::vector<Modulation> mods;mergePresetImportMods(modImport,mods,{{0x502,48,100,0,0,false}});require(mods.size()==1&&mods[0].amount==1060,"Preset velocity modulator lost default amount");
        auto imported=importSF2Program(libPath,2,4);require(imported.project.name=="First"&&imported.project.zones.size()==1&&imported.project.zones[0].low==60&&imported.project.samples[0].frames==8192,"Selected SF2 program import failed");
        auto drum=importSF2Program(libPath,128,0);require(drum.project.name=="Second"&&drum.project.zones[0].low==62,"SF2 drum bank import failed");
        auto rich=importSF2Program(modPath,0,0);const auto& importedZone=rich.project.zones[0];require(importedZone.loopMode==1&&importedZone.loopEnd-importedZone.loopStart==1919&&std::abs(importedZone.cutoff-2400)<=2&&importedZone.ampAttack==50&&importedZone.mods.size()==2,"SF2 loop, envelope, filter or modulation import failed");
        rejected=false;try{importSF2Program(libPath,2,5);}catch(...){rejected=true;}require(rejected,"Missing source program accepted");
        auto source=base/"loaded.sfz";{std::ofstream f(source);f<<"<global> sample=sample.wav ampeg_attack=0.05\n<group> lovel=10 hivel=100 cutoff=2400\n<region> key=C4 volume=6.25 amplitude=50 loop_mode=loop_continuous loop_start=16 loop_end=255 pitch_oncc1=50\n<region> key=D4 volume=8.75 seq_length=2 seq_position=2 sw_last=36\n";}
        int reads=0;auto sfzIn=importSFZProgram(source,[&](const Path& f){++reads;require(f.filename()=="sample.wav","SFZ path resolution failed");return ImportedAudio{{"sample.wav","wav",a,0,60,8192},-1,-1};});
        require(reads==1&&sfzIn.project.samples.size()==1&&sfzIn.project.zones.size()==2,"SFZ sample sharing or region import failed");const auto& sz=sfzIn.project.zones[0];require(sz.level==23&&sfzIn.project.zones[1].level==875&&std::abs(std::stod(zoneOps(sfzIn.project.zones[1],"sample.wav",Format::sfz).at("volume"))-8.75)<1e-8,"SFZ positive or fractional gain lost");require(sz.root==60&&sz.low==60&&sz.high==60&&sz.lowVel==10&&sz.highVel==100&&sz.ampAttack==50&&sz.cutoff==2400&&sz.loopStart==16&&sz.loopEnd==255&&sz.mods.size()==1,"SFZ mapping, envelope, loop or modulation failed");require(sfzIn.project.zones[1].seqPosition==2&&sfzIn.warnings.count("Unsupported SFZ opcode sw_last"),"SFZ articulation or unsupported opcode warning failed");
        std::cout<<"PASS: editable SF2/SFZ source import, selected MIDI addresses, loop/envelope/modulation and unsupported opcode diagnostics\n";
        std::cout<<"PASS: multi-program library, shared sample pool, address preservation and non-destructive SF2/SFZ export\n";
        std::cout<<"PASS: editor sample sharing, four mapping modes, range validation, non-destructive SFZ package and direct SF2 export\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
