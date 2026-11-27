#pragma once
#include <map>
#include <algorithm>
#include "VirtualProgramMap.h"
#include "AudioRouting.h"
namespace lsfx {
// Draft per-program note mixer state. No dependency on a synthesis engine.
// Output -1 inherits the channel route; 0 is Main. Level is tenths of dB.
struct NoteAssignment {
    int output=-1, level=0, pan=0, pitch=0;
    bool isDefault() const noexcept {return output==-1&&level==0&&pan==0&&pitch==0;}
};
using NoteAssignments=std::map<int,std::map<int,NoteAssignment>>;
inline NoteAssignment noteAssignment(const NoteAssignments& map,int code,int note) {
    auto p=map.find(code); if(p==map.end()) return {};
    auto n=p->second.find(note); return n==p->second.end()?NoteAssignment{}:n->second;
}
inline void setNoteAssignment(NoteAssignments& map,int code,int note,NoteAssignment v) {
    if(code<0||code>address(maxBank,127)||note<0||note>127) return;
    v.output=std::clamp(v.output,-1,maxOutputRoute); v.level=std::clamp(v.level,-600,240);
    v.pan=std::clamp(v.pan,-100,100); v.pitch=std::clamp(v.pitch,-1200,1200);
    if(v.isDefault()) {
        auto p=map.find(code); if(p!=map.end()) {p->second.erase(note);if(p->second.empty())map.erase(p);}
    } else map[code][note]=v;
}
}
