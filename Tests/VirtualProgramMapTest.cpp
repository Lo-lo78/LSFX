#include "../Source/VirtualProgramMap.h"
#include "../Source/NoteAssignments.h"
#include <cassert>
#include <string>
struct P {int bank,program,sourceBank,sourceProgram;std::string source;};
int main() {
    using namespace lsfx;
    static_assert(melodicBankCount==128&&programCount==128&&percussionBank==128);
    static_assert(validAddress(128,127)&&!validAddress(0,128)&&!validAddress(-1,0));
    std::map<int,P> map;
    std::vector<P> a{{0,0,0,0,"A"},{0,3,0,3,"A"},{128,0,128,0,"A"}};
    std::vector<P> b{{4,10,4,10,"B"},{4,20,4,20,"B"}};
    for(auto p:a) assignProgram(map,p,p.bank,p.program);
    assignProgram(map,b[0],0,3);
    assert(map.at(address(0,3)).source=="B"&&map.at(address(0,3)).sourceProgram==10);
    assert(map.at(address(0,0)).source=="A"&&map.at(address(128,0)).source=="A");
    assert(replaceBank(map,b,0,4));
    assert(map.count(address(0,0))==0&&map.count(address(0,3))==0);
    assert(map.at(address(0,10)).sourceBank==4&&map.at(address(0,20)).source=="B");
    assert(map.at(address(128,0)).source=="A");
    assert(!replaceBank(map,b,0,99)&&map.size()==3);
    assignProgram(map,a[0],127,127);
    assert(map.at(address(127,127)).sourceProgram==0);
    P sfz{0,0,0,0,"Piano.sfz"};
    assignProgram(map,sfz,0,10);
    assignProgram(map,P{0,0,0,0,"Bass.sfz"},1,3);
    assert(map.at(address(0,10)).source=="Piano.sfz");
    assert(map.at(address(0,20)).source=="B");
    assert(map.at(address(1,3)).source=="Bass.sfz");
    assignProgram(map,b[0],0,10);
    assert(map.at(address(0,10)).source=="B");
    assert(replaceBank(map,b,1,4));
    assert(map.count(address(1,3))==0);
    assert(map.at(address(1,10)).source=="B");
    assert(map.at(address(128,0)).source=="A");
    std::map<int,float> gains{{address(0,3),-6.0f},{address(0,4),12.0f},{address(1,3),3.0f},{address(128,0),-2.0f}};
    eraseInstrumentGains(gains,0,3);
    assert(!gains.count(address(0,3))&&gains.at(address(0,4))==12.0f);
    assert(gains.at(address(1,3))==3.0f);
    eraseInstrumentGains(gains,0,-1);
    assert(gains.size()==2&&gains.at(address(128,0))==-2.0f);
    eraseInstrumentGains(gains,1,3);
    assert(gains.size()==1&&gains.count(address(128,0)));
    // Tombstones applied in order must remove base or imported addresses without
    // revealing earlier layers. Subsequent import can intentionally refill them.
    std::map<int,P> layered;
    for(auto p:a) assignProgram(layered,p,p.bank,p.program);
    clearAddress(layered,0,3);
    assert(layered.count(address(0,0))&&!layered.count(address(0,3)));
    assignProgram(layered,b[0],0,3);
    clearAddress(layered,0,-1);
    assert(layered.size()==1&&layered.count(address(128,0)));
    assignProgram(layered,a[0],0,0);
    assert(layered.size()==2);
    clearAddress(layered,0,-2); // invalid request must not delete an entire bank
    assert(layered.size()==2);
    NoteAssignments notes;
    setNoteAssignment(notes,address(0,0),36,{1,20,-10,-20});
    setNoteAssignment(notes,address(0,0),38,{2,-10,0,0});
    setNoteAssignment(notes,address(128,0),36,{3,0,0,0});
    assert(noteAssignment(notes,address(0,0),36).output==1);
    assert(noteAssignment(notes,address(128,0),36).output==3);
    assert(noteAssignment(notes,address(0,0),37).isDefault());
    setNoteAssignment(notes,address(0,0),36,{});
    assert(notes.at(address(0,0)).size()==1);
    setNoteAssignment(notes,address(0,0),38,{100,-999,999,-9999});
    auto n=noteAssignment(notes,address(0,0),38);
    assert(n.output==maxOutputRoute&&n.level==-600&&n.pan==100&&n.pitch==-1200);
    static_assert(stereoOutputCount==64&&monoOutputCount==128);
    for(int route=-1;route<stereoOutputCount;++route) {
        setNoteAssignment(notes,address(0,0),38,{route,-10,0,0});
        assert(noteAssignment(notes,address(0,0),38).output==route);
    }
    clearAddress(notes,0,0);
    assert(notes.size()==1&&notes.count(address(128,0)));
    clearAddress(notes,128,-1);
    assert(notes.empty());
}
