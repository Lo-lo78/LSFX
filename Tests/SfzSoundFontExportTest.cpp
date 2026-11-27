#include "../Source/SfzSoundFontExport.h"
#include <iostream>
using namespace lsfx::sf2export;
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    try {
        Path root=argv[1],sf2=argv[2],out=argv[3];std::filesystem::create_directories(root);
        auto sfz=root/"test.sfz",part=root/"part.sfz",converted=root/"converted.sf2";
        {std::ofstream f(sfz);f<<"#define $sample tone sample.wav\n<control> default_path=Samples/\n<global> ampeg_release=0.25\n<group> sample=$sample pitch_keycenter=C4 loop_mode=loop_sustain loop_start=128 loop_end=895\n#include \"part.sfz\"\n";}
        {std::ofstream f(part);f<<"<region> lokey=48 hikey=71 lovel=1 hivel=63 amp_veltrack=0 volume=-3\n<region> lokey=48 hikey=71 lovel=64 hivel=127 cutoff=3000\n<region> trigger=release key=60\n";}
        int decodes=0;
        lsfx::sfzexport::Decode decode=[&](const Path& p){require(p.filename()=="tone sample.wav"&&p.parent_path().filename()=="Samples","Wrong SFZ sample path");++decodes;lsfx::sfzexport::Audio a;a.rate=44100;a.channels=2;for(int i=0;i<1024;++i){a.pcm.push_back(float(.25*std::sin(i*.1)));a.pcm.push_back(float(.15*std::sin(i*.13)));}return a;};
        lsfx::sfzexport::Report report;lsfx::sfzexport::convert(sfz,converted,decode,report);require(report.regions==2&&report.skipped==1&&!report.warnings.empty()&&decodes==1,"SFZ selection and warnings failed");
        Source c(converted);require(c.presets.size()==1&&c.sh.size()/46==3&&c.ib.size()/4==5,"Invalid stereo/velocity region tables");require(u16(c.sh,44)==4&&u16(c.sh,46+44)==2&&u16(c.sh,42)==1&&u16(c.sh,46+42)==0,"Wrong stereo sample links");
        require(u32(c.sh,28)-u32(c.sh,20)==128&&u32(c.sh,32)-u32(c.sh,20)==896,"Loop points lost");
        require(u16(c.ig,0)==43&&u16(c.ig,2)==(48|(71<<8))&&u16(c.ig,4)==44&&u16(c.ig,6)==(1|(63<<8)),"Key or velocity range lost");
        auto r=exportFile({{sf2,0,0,0,0},{converted,0,0,0,3},{converted,0,0,1,7},{sf2,128,0,128,0}},out);Source result(out);require(r.presets==4&&r.samples==3&&result.presets.count({0,3})&&result.presets.count({1,7}),"Mixed SFZ and SF2 export failed");
        std::atomic<bool> cancel{true};bool rejected=false;try{lsfx::sfzexport::Report tmp;lsfx::sfzexport::convert(sfz,root/"cancel.sf2",decode,tmp,&cancel);}catch(const std::exception&){rejected=true;}require(rejected,"Cancellation ignored");
        std::cout<<"PASS: SFZ include, macro, path, inheritance, stereo samples, velocity layers, loop, warnings, mixed SF2 export and cancellation\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
