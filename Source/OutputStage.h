#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>

namespace lsfx {
// Adapted from LJuno-116 0.99.67 SynthEngine.cpp finalizer (stereo master).
struct OutputStage {
    float envelope=0, attack=1, release=1;
    void prepare(double rate) noexcept { attack=1.0f/std::max(1.0f,float(rate*.001)); release=1.0f/std::max(1.0f,float(rate*.004)); reset(); }
    void reset() noexcept { envelope=0; }
    static float limit(float x) noexcept { const auto a=std::abs(x); return a>0.98f?std::copysign(0.98f+(a-0.98f)*0.15f,x):x; }
    void process(float& l,float& r,float gain,float trim) noexcept {
        l=std::isfinite(l)?l*trim:0; r=std::isfinite(r)?r*trim:0;
        const float detector=std::sqrt(std::max(l*l,r*r)+0.000000000001f);
        envelope+=(detector-envelope)*(envelope<detector?attack:release);
        const float scale=0.4f*(envelope>0.055f?(0.055f+(envelope-0.055f)*0.5f)/envelope:1.0f);
        l=limit(l*scale)*gain*2.0f; r=limit(r*scale)*gain*2.0f;
    }
};
// Allocation only during loading. Reset is O(1), stale samples are inaccessible.
struct StereoDelay {
    std::vector<float> left,right;
    int head=0, valid=0; double rate=44100; float phase=0;
    void prepare(double sr) { rate=sr; left.assign(std::size_t(sr*1.1)+8,0); right=left; reset(); }
    void reset() noexcept { head=valid=0; phase=0; }
    float tap(const std::vector<float>& line,float delay) const noexcept {
        delay=std::clamp(delay,1.0f,float(line.size()-2)); const int d=int(delay); const float f=delay-d;
        const int n=int(line.size());
        const float a=d<=valid?line[(head+n-d)%n]:0.0f;
        const float b=d+1<=valid?line[(head+n-d-1)%n]:0.0f;
        return a+(b-a)*f;
    }
    void process(float inL,float inR,float ms,float depth,float hz,float feedback,bool pingPong,float& wetL,float& wetR) noexcept {
        if(left.empty()) { wetL=wetR=0; return; }
        const float base=ms*float(rate*.001), swing=depth*float(rate*.001);
        const float dl=depth>0?base+swing*std::sin(phase):base, dr=depth>0?base+swing*std::sin(phase+1.570796327f):base;
        wetL=tap(left,dl); wetR=tap(right,dr);
        feedback=std::clamp(feedback,-0.85f,0.85f);
        auto clean=[](float x){return std::abs(x)<1.0e-15f?0.0f:x;};
        left[head]=clean(inL+feedback*(pingPong?wetR:wetL)); right[head]=clean(inR+feedback*(pingPong?wetL:wetR));
        if(++head==int(left.size())) head=0;
        valid=std::min(valid+1,int(left.size())-1);
        phase+=hz*6.283185307f/float(rate); if(phase>=6.283185307f) phase-=6.283185307f;
    }
};
}
