#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace lsfx {
// Stereo-linked, 1176-inspired digital parallel compressor. Parameter ranges
// and RMS detector/envelope model follow LJuno-116 0.99.67. No fixed gain or
// legacy attenuation in this new path. Mix zero is a bit-transparent bypass.
enum CompParam { compThreshold, compRatio, compAttack, compRelease, compMakeup, compMix, compCount };
struct CompSpec { const char* key; const char* name; float lo,hi,step,initial; const char* unit; };
inline constexpr CompSpec compSpecs[] = {
    {"threshold","Compressor threshold",-60,0,0.1f,-11," dB"},
    {"ratio","Compressor ratio",0,3,1,0,""},
    {"attack","Compressor attack",20,2000,10,1000," us"},
    {"release","Compressor release",20,2000,1,250," ms"},
    {"makeup","Compressor gain",-36,36,0.1f,7," dB"},
    {"mix","Compressor mix",0,100,0.1f,0," %"}
};
inline constexpr const char* compRatioNames[]={"4:1","8:1","12:1","20:1"};
using CompValues=std::array<float,compCount>;
struct ChannelCompressor {
    double rate=44100;
    float average=0, over=0, rms=0, attack=1, release=1, ratio=0.75f, threshold=-11, makeup=7, mix=0;
    CompValues last{};
    bool configured=false;
    void prepare(double sr) noexcept {
        rate=(std::max)(1.0,sr); rms=float(std::exp(-1.0/(rate*.01))); configured=false; reset();
    }
    void reset() noexcept { average=over=0; }
    void configure(CompValues p) noexcept {
        for(int i=0;i<compCount;++i) {
            const auto& s=compSpecs[i];
            p[i]=std::isfinite(p[i])?std::clamp(p[i],s.lo,s.hi):s.initial;
        }
        if(configured&&p==last) return;
        last=p; configured=true;
        if(p[compMix]==0||mix==0) reset();
        constexpr float ratios[]={4,8,12,20};
        ratio=1.0f-1.0f/ratios[int(std::lround(p[compRatio]))];
        attack=float(-std::expm1(-1.0/(rate*p[compAttack]*.000001)));
        release=float(-std::expm1(-1.0/(rate*p[compRelease]*.001)));
        threshold=p[compThreshold]; makeup=p[compMakeup]; mix=p[compMix]*.01f;
    }
    // One detector for the sum of all outputs of the original MIDI channel;
    // apply the same gain to each stereo pair, preserving independent routing.
    void processSplit(float* const* left,float* const* right,int pairs,int n) noexcept {
        if(mix<=0||pairs<=0)return;
        if(pairs==1){process(left[0],right[0],n);return;}
        constexpr float dbToLog=0.1151292546497023f;
        for(int i=0;i<n;++i) {
            float l=0,r=0;
            for(int p=0;p<pairs;++p) {
                auto& a=left[p][i];auto& b=right[p][i];
                if(!std::isfinite(a))a=0;if(!std::isfinite(b))b=0;
                l+=a;r+=b;
            }
            const float peak=(std::min)((std::max)(std::abs(l),std::abs(r)),1000.0f);
            const float squared=peak*peak;
            average=squared+rms*(average-squared);
            if(average<1e-18f)average=0;
            const float target=(std::max)(0.0f,10.0f*std::log10((std::max)(1e-18f,average))-threshold);
            over+=(target-over)*(target>over?attack:release);
            if(over<1e-12f)over=0;
            const float scale=1.0f+mix*(std::exp((makeup-over*ratio)*dbToLog)-1.0f);
            for(int p=0;p<pairs;++p){left[p][i]*=scale;right[p][i]*=scale;}
        }
    }
    void process(float* l,float* r,int n) noexcept {
        if(mix<=0) return;
        constexpr float dbToLog=0.1151292546497023f;
        for(int i=0;i<n;++i) {
            const float a=std::isfinite(l[i])?l[i]:0.0f, b=std::isfinite(r[i])?r[i]:0.0f;
            const float peak=(std::max)(std::abs(a),std::abs(b));
            // Avoid overflow even if malformed source samples are encountered.
            const float limited=(std::min)(peak,1000.0f), squared=limited*limited;
            average=squared+rms*(average-squared);
            if(average<1e-18f) average=0;
            const float target=(std::max)(0.0f,10.0f*std::log10((std::max)(1e-18f,average))-threshold);
            over+=(target-over)*(target>over?attack:release);
            if(over<1e-12f) over=0;
            const float wet=std::exp((makeup-over*ratio)*dbToLog);
            const float scale=1.0f+mix*(wet-1.0f);
            l[i]=a*scale; r[i]=b*scale;
        }
    }
};
}
