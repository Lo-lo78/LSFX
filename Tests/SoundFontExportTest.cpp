// Standalone structural tests. CI additionally loads/renders the generated SF2
// using the actual linked FluidSynth (separate process).
#include "../Source/SoundFontExport.h"
#include <iostream>
using namespace lsfx::sf2export;
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    try {
        Path a=argv[1],b=argv[2],out=argv[3];
        std::vector<Entry> map{{a,0,0,0,0},{a,0,0,3,7},{b,128,0,128,0}};
        auto r=exportFile(map,out);
        require(r.presets==3&&r.sources==2&&r.instruments==2&&r.samples==2,"Dependency reuse or source selection failed");
        Source result(out);require(result.presets.size()==3&&result.presets.count({0,0})&&result.presets.count({3,7})&&result.presets.count({128,0}),"Wrong destination map");
        require(!result.presets.count({0,1}),"Unexpected program");
        require(result.ih.size()/22==3&&result.sh.size()/46==3,"Wrong terminal records");
        require(std::filesystem::file_size(out)==r.bytes,"Wrong RIFF size");
        Source original(a);auto n=u32(original.sh,24)-u32(original.sh,20);auto start=u32(result.sh,20);
        require(result.read(result.pcm.pos+2*start,2*n)==original.read(original.pcm.pos+2*u32(original.sh,20),2*n),"PCM changed");
        auto reject=[&](std::vector<Entry> bad,const std::atomic<bool>* c=nullptr) {
            auto failed=out;failed+=".rejected";std::filesystem::remove(failed);bool rejected=false;
            try{exportFile(std::move(bad),failed,c);}catch(const std::exception&){rejected=true;}
            require(rejected&&!std::filesystem::exists(failed),"Invalid map created a file");
        };
        reject({});reject({{a,0,0,129,0}});reject({{a,0,99,0,0}});reject({{a,0,0,0,0},{b,0,0,0,0}});
        std::atomic<bool> cancelled{true};reject(map,&cancelled);
        std::cout<<"PASS: selective SF2 export, remapping, shared dependencies, exact PCM, invalid map and cancellation guards\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
