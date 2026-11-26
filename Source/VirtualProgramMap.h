#pragma once
#include <map>
#include <vector>
#include <utility>
namespace lsfx {
constexpr int programCount=128;
constexpr int melodicBankCount=128;
constexpr int percussionBank=melodicBankCount;
constexpr int maxBank=16383;
constexpr int address(int bank,int program) noexcept {return bank*programCount+program;}
constexpr bool validAddress(int bank,int program) noexcept {
    return bank>=0&&bank<=maxBank&&program>=0&&program<programCount;
}
// All sample data and source coordinates remain unchanged. Only the virtual
// address of the metadata entry is changed. Called on the loader, never RT.
template<class Preset> void assignProgram(std::map<int,Preset>& map,Preset p,int bank,int program) {
    p.bank=bank;p.program=program;map[address(bank,program)]=std::move(p);
}
template<class Preset> bool replaceBank(std::map<int,Preset>& map,const std::vector<Preset>& source,int bank,int sourceBank) {
    bool found=false;
    for(const auto& p:source) if(p.bank==sourceBank) {found=true;break;}
    if(!found) return false;
    auto it=map.lower_bound(address(bank,0));
    while(it!=map.end()&&it->first/programCount==bank) it=map.erase(it);
    for(const auto& p:source) if(p.bank==sourceBank) assignProgram(map,p,bank,p.program);
    return true;
}
}
