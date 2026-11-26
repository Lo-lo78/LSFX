#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace lsfx {
// Decode complete MIDI SysEx messages (with F0/F7). No allocations or I/O.
// The caller owns effect state. GS/XG parameters are translated, not emulated bit-for-bit.
enum class MidiFx { reset, reverbType, chorusType, reverbLevel, reverbTime, reverbFeedback,
                    chorusLevel, chorusRate, chorusDepth, chorusFeedback, delayLevel,
                    delayTime, delayFeedback, delayMode, reverbEnable, chorusEnable, send, masterVolume, partVolume };
inline int percent(int v) { return (v*100+63)/127; }
inline int feedback(int v) { return (v*85+63)/127; }
inline int gsDelayMs(int v) {
    if(v<=20) return (std::max)(1,(v+5)/10);
    if(v<=35) return (20+(v-20)*2+5)/10;
    if(v<=45) return 5+(v-35)/2;
    if(v<=55) return 10+v-45;
    if(v<=70) return 20+2*(v-55);
    if(v<=80) return 50+5*(v-70);
    if(v<=90) return 100+10*(v-80);
    if(v<=105) return 200+20*(v-90);
    return (std::min)(1000,500+50*(v-105));
}
template<class Emit> bool decodeMidiEffects(const std::uint8_t* d,int n,Emit&& emit) {
    if(n<4||d[0]!=0xf0||d[n-1]!=0xf7) return false;
    for(int i=1;i<n-1;++i) if(d[i]>=128) return false;
    bool handled=false;
    auto put=[&](MidiFx k,int value,int channel=-1,int cc=-1){ emit(k,value,channel,cc); handled=true; };
    if(n==6 && d[1]==0x7e && d[3]==9 && (d[4]==1||d[4]==3)) put(MidiFx::reset,0);
    else if(n==8 && d[1]==0x7f && d[3]==4 && d[4]==1) put(MidiFx::masterVolume,d[5]|(d[6]<<7));
    else if(n>=11 && d[1]==0x41 && d[3]==0x42 && d[4]==0x12) {
        int sum=0; for(int i=5;i<n-1;++i) sum+=d[i]; if(sum%128) return false;
        const int high=d[5], mid=d[6], start=d[7];
        for(int i=8;i<n-2;++i) {
            const int addr=start+i-8, v=d[i];
            if(high!=0x40 || addr>127) continue;
            if(mid==0 && addr==0x7f && v==0) put(MidiFx::reset,0);
            else if(mid==0 && addr==4) put(MidiFx::masterVolume,(v*16383+63)/127);
            else if(mid==1) switch(addr) {
                case 0x30: case 0x31: if(v<8) {
                    put(MidiFx::reverbType,v);put(MidiFx::reverbEnable,1);
                    if(addr==0x30) {
                        constexpr int time[]={80,56,64,72,64,88,32,64}, fb[]={0,0,0,0,0,0,40,32};
                        put(MidiFx::reverbLevel,percent(64));put(MidiFx::reverbTime,percent(time[v]));
                        if(v>=6) put(MidiFx::reverbFeedback,feedback(fb[v]));
                    }
                } break;
                case 0x33: put(MidiFx::reverbLevel,percent(v)); break;
                case 0x34: put(MidiFx::reverbTime,percent(v)); break;
                case 0x35: put(MidiFx::reverbFeedback,feedback(v)); break;
                case 0x38: if(v<8) {
                    constexpr int fb[]={0,5,8,16,64,112,0,80}, rate[]={3,9,3,9,2,1,0,0}, depth[]={5,19,19,16,24,5,127,127};
                    put(MidiFx::chorusType,v);put(MidiFx::chorusEnable,1);put(MidiFx::chorusLevel,percent(64));
                    put(MidiFx::chorusFeedback,feedback(fb[v]));put(MidiFx::chorusRate,10+(rate[v]*490+63)/127);put(MidiFx::chorusDepth,percent(depth[v]));
                } break;
                case 0x3a: put(MidiFx::chorusLevel,percent(v)); break;
                case 0x3b: put(MidiFx::chorusFeedback,feedback(v)); break;
                case 0x3d: put(MidiFx::chorusRate,10+(v*490+63)/127); break;
                case 0x3e: put(MidiFx::chorusDepth,percent(v)); break;
                case 0x50: if(v<=9) {put(MidiFx::delayMode,v>=4?1:0);put(MidiFx::delayLevel,50);} break;
                case 0x52: put(MidiFx::delayTime,gsDelayMs(v)); break;
                case 0x58: put(MidiFx::delayLevel,percent(v)); break;
                case 0x59: put(MidiFx::delayFeedback,feedback(std::abs(v-64)*2>127?127:std::abs(v-64)*2)); break;
                default: break;
            }
            // GS default part order: part 0 is MIDI channel 10; 1..9 are ch1..9.
            else if((mid&0x70)==0x10 && (addr==0x19||addr==0x21||addr==0x22)) {
                const int part=mid&15, ch=part==0?9:part<=9?part-1:part;
                if(addr==0x19) put(MidiFx::partVolume,v,ch,7);
                else put(MidiFx::send,v,ch,addr==0x21?93:91);
            }
        }
    } else if(n>=8 && d[1]==0x43 && (d[2]&0x70)==0x10 && d[3]==0x4c) {
        const int high=d[4], mid=d[5], addr=d[6], v=d[7];
        if(n<9) return false;
        if(high==0 && mid==0 && addr==0x7e && v==0) put(MidiFx::reset,0);
        else if(high==0 && mid==0 && addr==4) put(MidiFx::masterVolume,(v*16383+63)/127);
        else if(high==2 && mid==1) {
            if((addr==0||addr==0x20)&&n>=10) {
                const int lsb=d[8];
                if(addr==0) {
                    if(v==0) put(MidiFx::reverbEnable,0);
                    else if(v>=1&&v<=4) { put(MidiFx::reverbType,v==1?3+(lsb!=0):v==2?(std::min)(2,int(lsb)):v==3?3+(lsb!=0):5); put(MidiFx::reverbEnable,1); }
                } else {
                    if(v==0) put(MidiFx::chorusEnable,0);
                    else if(v>=0x41&&v<=0x43) { put(MidiFx::chorusType,v==0x43?5:lsb==8?3:(std::min)(3,int(lsb))); put(MidiFx::chorusEnable,1); }
                }
            } else if(addr==0x0c) put(MidiFx::reverbLevel,percent(v));
            else if(addr==0x2c) put(MidiFx::chorusLevel,percent(v));
        } else if(high==8 && mid<16 && addr==0x0b) put(MidiFx::partVolume,v,mid,7);
        else if(high==8 && mid<16 && (addr==0x12||addr==0x13)) put(MidiFx::send,v,mid,addr==0x12?93:91);
    }
    return handled;
}
}
