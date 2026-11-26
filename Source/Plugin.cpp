#include <JuceHeader.h>
#include <fluidsynth.h>
#include <sfizz.h>
#include "OutputStage.h"
#include "MidiEffects.h"
#include "ParameterSearchKey.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#if JUCE_WINDOWS
 #include <Windows.h>
 #include <UIAutomation.h>
 #include <oleauto.h>
#endif

namespace {
constexpr int channels = 16;
// Stable indices for global host parameters and XML attributes.
enum FXParam { rvType, rvLevel, rvTime, rvDamp, rvWidth, chType, chLevel, chRate, chDepth, chFeedback, delayMode, delayTime, delayFeedback, delayMix, inputTrim, fxCount };
struct FXSpec { const char* key; const char* label; int lo,hi,initial; const char* unit; };
constexpr FXSpec fxSpecs[] = {
    {"reverbType","Reverb type",0,7,4,""}, {"reverbLevel","Reverb level",0,100,70," %"}, {"reverbTime","Reverb time scale",0,100,50," %"}, {"reverbDamp","Reverb damping",0,100,30," %"}, {"reverbWidth","Reverb width",0,100,80," %"},
    {"chorusType","Chorus type",0,7,0,""}, {"chorusLevel","Chorus level",0,100,35," %"}, {"chorusRate","Chorus rate",10,500,30," x 0.01 Hz"}, {"chorusDepth","Chorus depth scale",0,100,50," %"}, {"chorusFeedback","Chorus feedback",0,85,25," %"},
    {"delayMode","Delay mode",0,1,0,""}, {"delayTime","Delay time",1,1000,250," ms"}, {"delayFeedback","Delay feedback",0,85,30," %"}, {"delayMix","Delay level",0,100,0," %"}, {"inputTrim","Output stage input trim",0,24,12," dB"}
};
using FXValues=std::array<int,fxCount>;
constexpr int globalRowBase=-100;
int fxRow(int i) { return globalRowBase-i; }
int fxIndex(int row) { const int i=globalRowBase-row; return i>=0&&i<fxCount?i:-1; }
const char* const reverbNames[]={"Room 1","Room 2","Room 3","Hall 1","Hall 2","Plate","Delay","Panning Delay"};
const char* const chorusNames[]={"Chorus 1","Chorus 2","Chorus 3","Chorus 4","Feedback Chorus","Flanger","Short Delay","Short Delay with Feedback"};
float roomSize(const FXValues& f) { constexpr float rooms[]={.22f,.36f,.49f,.62f,.76f,.57f}; return std::clamp(rooms[(std::min)(f[rvType],5)]*(.5f+.01f*f[rvTime]),.02f,.88f); }
float chorusDepthMs(const FXValues& f) { constexpr float depths[]={2,4,6,9,3,1.5f,0,0}; return depths[f[chType]]*(.01f*f[chDepth]*2); }

constexpr int pack(int bank, int program) { return (bank << 7) | program; }
constexpr int unset = -32768;
struct ControlSpec { int id; const char* name; int lo,hi,step,gen; const char* unit; };
constexpr ControlSpec sf2Controls[] = {
    {7,"Channel volume (CC7)",0,127,1,-1,""}, {10,"Pan (CC10)",0,127,1,-1,""},
    {11,"Expression (CC11)",0,127,1,-1,""}, {1,"Modulation (CC1)",0,127,1,-1,""},
    {91,"Reverb send (CC91)",0,127,1,-1,""}, {93,"Chorus send (CC93)",0,127,1,-1,""},
    {128,"Filter cutoff offset",-12000,12000,100,GEN_FILTERFC," cents"},
    {129,"Filter resonance offset",-960,960,10,GEN_FILTERQ," cB"},
    {130,"Volume attack offset",-12000,12000,100,GEN_VOLENVATTACK," timecents"},
    {131,"Volume hold offset",-12000,12000,100,GEN_VOLENVHOLD," timecents"},
    {132,"Volume decay offset",-12000,12000,100,GEN_VOLENVDECAY," timecents"},
    {133,"Volume sustain attenuation offset",-1000,1000,10,GEN_VOLENVSUSTAIN," cB"},
    {134,"Volume release offset",-12000,12000,100,GEN_VOLENVRELEASE," timecents"},
    {135,"Coarse tune offset",-24,24,1,GEN_COARSETUNE," semitones"},
    {136,"Fine tune offset",-100,100,1,GEN_FINETUNE," cents"},
    {137,"Vibrato depth offset",-1200,1200,10,GEN_VIBLFOTOPITCH," cents"},
    {138,"Modulation LFO to filter offset",-12000,12000,100,GEN_MODLFOTOFILTERFC," cents"},
    {139,"Modulation envelope to filter offset",-12000,12000,100,GEN_MODENVTOFILTERFC," cents"}
};
constexpr int slots=140, controlCount=channels*slots;
using Controls=std::array<int,controlCount>;
Controls emptyControls() { Controls c; c.fill(unset); return c; }
int controlIndex(int ch,int id) { return ch*slots+id; }
const ControlSpec* sf2Spec(int id) { for(const auto& c:sf2Controls) if(c.id==id) return &c; return nullptr; }
struct CCLabel { int cc; juce::String name; int initial=unset; };
struct Preset { int bank, program; juce::String name; };
struct Engine {
    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    sfizz_synth_t* sfz = nullptr;
    int quality = -1, effects = -1;
    int quietSamples = 0;
    int midiDisabledFx=0;
    int lastMidiFxMode=-1;
    float midiMaster=1.0f;
    FXValues lastFx{};
    lsfx::OutputStage output;
    lsfx::StereoDelay reverbDelay, modulation, delay;
    juce::Reverb sfzReverb;
    void prepareFX() { lastFx.fill(-1); output.prepare(rate); reverbDelay.prepare(rate); modulation.prepare(rate); delay.prepare(rate); sfzReverb.setSampleRate(rate); }
    void resetFX() { output.reset(); reverbDelay.reset(); modulation.reset(); delay.reset(); sfzReverb.reset(); quietSamples=0; }

    int sfzDefaultQuality=0, sfzAppliedQuality=-1;
    int font = -1;
    double rate = 44100;
    ~Engine() { if (sfz) sfizz_free(sfz); if (synth) delete_fluid_synth(synth); if (settings) delete_fluid_settings(settings); }
};
using Programs = std::array<int, channels>;
Programs defaults() { Programs p{}; p[9] = pack(128, 0); return p; }
struct Request { juce::String path; double rate = 44100; Programs programs = defaults(); uint64_t serial = 0; Controls controls=emptyControls(); };
struct View { juce::String path, status = "No SoundFont loaded"; std::vector<Preset> presets; uint64_t revision = 0; bool loading = false; bool isSfz = false; bool ready=false; std::vector<CCLabel> labels; };
}

class Processor final : public juce::AudioProcessor {
public:
    Processor() : AudioProcessor(BusesProperties().withOutput("Stereo", juce::AudioChannelSet::stereo(), true)) {
        auto p = defaults();
        for (int i=0; i<channels; ++i) { selected[i].store(p[i]); commands[i].store(-1); }
        addParameter(gain = new juce::AudioParameterFloat({"gain",1}, "Output gain", juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), -6.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
        addParameter(quality = new juce::AudioParameterChoice({"quality",1}, "SF2 interpolation", {"Linear", "Fourth order", "Seventh order"}, 1));
        addParameter(effects = new juce::AudioParameterChoice({"effects",1}, "Effects", {"Off", "Reverb", "Chorus", "Both"}, 0));
        addParameter(sfzQuality = new juce::AudioParameterChoice({"sfzQuality",1}, "SFZ live sample quality", {"Engine default","0","1","2","3","4","5","6","7","8","9","10"},0));
        for(int i=0;i<fxCount;++i) { const auto& f=fxSpecs[i];
            if(i==rvType||i==chType||i==delayMode) {
                juce::StringArray choices;
                if(i==delayMode) { choices.add("Stereo"); choices.add("Ping-pong"); }
                else for(int k=0;k<8;++k) choices.add(i==rvType?reverbNames[k]:chorusNames[k]);
                addParameter(fxParams[i]=new juce::AudioParameterChoice({f.key,1},f.label,choices,f.initial));
            } else addParameter(fxParams[i]=new juce::AudioParameterInt({f.key,1},f.label,f.lo,f.hi,f.initial));
        }
        addParameter(midiFX = new juce::AudioParameterChoice({"midiFX",1}, "MIDI effects reception", {"Manual", "Follow MIDI"},1));
        for(int i=0;i<controlCount;++i) { controlValues[i].store(unset); controlCommands[i].store(unset); }
        worker = std::thread([this]{ work(); });
    }
    ~Processor() override {
        { std::lock_guard<std::mutex> lock(requestMutex); stopping = true; }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }
    const juce::String getName() const override { return "LSFX"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override {
        const auto f=readFX();
        if(f[delayMix]==0&&(!(effects->getIndex()&1)||f[rvType]<6)) return 10.0;
        const double fb=f[delayFeedback]*.01;
        const double repeats=fb>0?std::ceil(std::log(0.00001)/std::log(fb)):1.0;
        return (std::max)(10.0, f[delayTime]*.001*(repeats+1.0));
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Current state"; }
    void changeProgramName(int,const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool isBusesLayoutSupported(const BusesLayout& l) const override { return l.getMainOutputChannelSet()==juce::AudioChannelSet::stereo(); }
    void prepareToPlay(double rate, int) override {
        const auto old = hostRate.exchange(rate);
        if (std::abs(old-rate)>0.01) {
            juce::String path; Programs p; Controls c;
            stateForSave(path,p,&c);
            if (path.isNotEmpty()) load(path,p,c);
        }
    }
    void releaseResources() override { panic.store(true); }
    void reset() override { panic.store(true); }
    uint64_t getRevision() { std::lock_guard<std::mutex> lock(viewMutex); return view.revision; }
    View getView() { std::lock_guard<std::mutex> lock(viewMutex); return view; }
    int current(int ch) const { const auto cmd=commands[ch].load(); return cmd>=0 ? cmd : selected[ch].load(); }
    void select(int ch,int bank,int prog) { if (ch>=0 && ch<channels && bank>=0 && bank<=16383 && prog>=0 && prog<128) commands[ch].store(pack(bank,prog)); }
    void allOff() { panic.store(true); }
    void loadFile(const juce::File& f) {
        if(f.hasFileExtension("sf2;sf3") && midiFX->getIndex()==1 && effects->getIndex()==0)
            effects->setValueNotifyingHost(effects->convertTo0to1(3.0f));
        load(f.getFullPathName(), defaults());
    }
    juce::AudioParameterFloat& gainParameter() { return *gain; }
    juce::AudioParameterChoice& qualityParameter() { return *quality; }
    juce::AudioParameterChoice& effectsParameter() { return *effects; }
    juce::AudioParameterChoice& midiFXParameter() { return *midiFX; }
    juce::AudioParameterChoice& sfzQualityParameter() { return *sfzQuality; }
    juce::RangedAudioParameter& fxParameter(int i) { return *fxParams[i]; }
    juce::String diagnostic() const { return "Active voices: "+juce::String(activeVoices.load())+"; SF2 idle sleep: "+(sleeping.load()?juce::String("yes"):juce::String("no"))+"; dropped engine-swap blocks: "+juce::String(droppedBlocks.load())+"; MIDI program changes: "+juce::String(programChanges.load())+"; last program change: "+lastProgramText()+"; MIDI CC91: "+juce::String(reverbCC.load())+"; MIDI CC93: "+juce::String(chorusCC.load())+"; SysEx received: "+juce::String(sysexCount.load())+"; FX SysEx applied: "+juce::String(fxSysexCount.load()); }
    int getControl(int ch,int id) const {
        const int i=controlIndex(ch,id), pendingValue=controlCommands[i].load();
        return pendingValue==unset?controlValues[i].load():pendingValue;
    }
    void setControl(int ch,int id,int value) {
        if(ch<0||ch>=channels||id<0||id>=slots) return;
        if(id<128) { if(id==0||id==32||id>=120) return; value=juce::jlimit(0,127,value); }
        else if(auto* spec=sf2Spec(id)) value=juce::jlimit(spec->lo,spec->hi,value); else return;
        std::lock_guard<std::mutex> lock(requestMutex);
        if(pending||loadingSerial!=0||failedSerial==latest.serial||latest.path.isEmpty()) return;
        const int i=controlIndex(ch,id); overrides[i]=value;
        controlCommands[i].store(value); controlDirty.store(true);
    }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        b.clear();
        if(b.getNumSamples()==0) { midi.clear(); return; }
        // No waiting for disk I/O or for the worker on the audio thread.
        std::unique_lock<std::mutex> lock(engineMutex, std::try_to_lock);
        if (!lock.owns_lock()) { ++droppedBlocks; for(const auto m:midi) if(m.numBytes>0&&m.data[0]<0xf8) { panic.store(true); break; } midi.clear(); return; }
        if (!engine || std::abs(engine->rate-hostRate.load())>0.01 || b.getNumChannels()<2) { activeVoices.store(0); sleeping.store(false); midi.clear(); return; }
        auto f=readFX(); int fx=effects->getIndex();
        const int followMode=midiFX->getIndex();
        if(engine->lastMidiFxMode!=followMode) {engine->midiDisabledFx=0; engine->lastMidiFxMode=followMode;}
        const bool settingsChanged=configureFX(*engine,f,fx);
        if (engine->sfz) {
            auto* z=engine->sfz;
            const int sq=sfzQuality->getIndex();
            if(sq!=engine->sfzAppliedQuality) {
                if(sq!=0 || engine->sfzAppliedQuality!=-1) sfizz_set_sample_quality(z,SFIZZ_PROCESS_LIVE,sq==0?engine->sfzDefaultQuality:sq-1);
                engine->sfzAppliedQuality=sq;
            }
            applyControls(*engine);
            const bool p=panic.exchange(false);
            if(p) { engine->resetFX(); controlValues[controlIndex(0,64)].store(0); sfizz_send_cc(z,0,64,0); sfizz_send_cc(z,0,120,0); sfizz_send_cc(z,0,123,0); for(int key=0;key<128;++key) sfizz_send_note_off(z,0,key,0); }
            // Fixed-size chunks guarantee the configured maximum block size.
            auto it=midi.begin(); const auto end=midi.end();
            for(int start=0;start<b.getNumSamples();) {
                const int count=juce::jmin(1024,b.getNumSamples()-start);
                while(it!=end) {
                    const auto m=*it;
                    if(m.samplePosition>=start+count) break;
                    ++it;
                    if(m.numBytes<2 || (m.data[0]&15)!=0) continue;
                    const int st=m.data[0]&240, a=m.data[1]&127, v=m.numBytes>=3?m.data[2]&127:0;
                    const int t=juce::jlimit(0,count-1,m.samplePosition-start);
                    if(st==0x90 && m.numBytes>=3 && v) sfizz_send_note_on(z,t,a,v);
                    else if((st==0x80 || st==0x90) && m.numBytes>=3) sfizz_send_note_off(z,t,a,v);
                    else if(st==0xb0 && m.numBytes>=3) { sfizz_send_cc(z,t,a,v); if(a<120) controlValues[controlIndex(0,a)].store(v); }
                    else if(st==0xe0 && m.numBytes>=3) sfizz_send_hd_pitch_wheel(z,t,((a|(v<<7))-8192)/8192.0f);
                    else if(st==0xc0) { receiveProgram(0,a); sfizz_send_program_change(z,t,a); }
                }
                float* out[]={b.getWritePointer(0,start),b.getWritePointer(1,start)};
                sfizz_render_block(z,out,2,count); start+=count;
            }
            activeVoices.store(sfizz_get_num_active_voices(z)); sleeping.store(false);
            processMaster(*engine,b,f,fx); midi.clear(); return;
        }
        auto* s = engine->synth;
        const int q=quality->getIndex();
        bool controlChanged = q!=engine->quality || settingsChanged;
        if(applyControls(*engine)) controlChanged=true;
        if(q!=engine->quality) { const int methods[]={FLUID_INTERP_LINEAR,FLUID_INTERP_4THORDER,FLUID_INTERP_7THORDER}; fluid_synth_set_interp_method(s,-1,methods[q]); engine->quality=q; }
        if (panic.exchange(false)) { controlChanged=true; engine->resetFX(); for (int ch=0;ch<channels;++ch) { fluid_synth_cc(s,ch,64,0); fluid_synth_cc(s,ch,66,0); controlValues[controlIndex(ch,64)].store(0); controlValues[controlIndex(ch,66)].store(0); fluid_synth_all_sounds_off(s,ch); } }
        for (int ch=0; ch<channels; ++ch) {
            const int cmd = commands[ch].exchange(-1);
            if (cmd>=0) { controlChanged=true; if(fluid_synth_program_select(s,ch,engine->font,cmd>>7,cmd&127)==FLUID_OK) selected[ch].store(cmd); }
        }
        // MIDI clock / sensing do not need to wake the synthesizer.
        bool relevantMidi=false;
        for(const auto m:midi) if(m.numBytes>0&&m.data[0]<0xf8) { relevantMidi=true; break; }
        const int before=fluid_synth_get_active_voice_count(s);
        const int sleepAfter=int(engine->rate*2.0);
        if(!controlChanged&&!relevantMidi&&before==0&&engine->quietSamples>=sleepAfter) { activeVoices.store(0); sleeping.store(true); midi.clear(); return; }
        sleeping.store(false);
        if(controlChanged||relevantMidi||before>0) engine->quietSamples=0;
        // FluidSynth renders each section preceding the corresponding MIDI event.
        // Its internal block size may still quantise event timing slightly.
        float rawPeak=0, wetPeak=0;
        auto render = [&](int start,int count) {
            if(count<=0) return;
            fluid_synth_write_float(s,count,b.getWritePointer(0,start),0,1,b.getWritePointer(1,start),0,1);
            rawPeak=juce::jmax(rawPeak,juce::jmax(b.getMagnitude(0,start,count),b.getMagnitude(1,start,count)));
            wetPeak=juce::jmax(wetPeak,processMaster(*engine,b,f,fx,start,count));
        };
        int cursor=0;
        // VST3 delivers controllers in separate queues and SysEx/notes in an event list.
        // Restore setup ordering within each timestamp: SysEx, Bank Select, Program,
        // then other controls and notes. No allocation or reordering across timestamps.
        auto handleMidi = [&](const auto& meta) {
            if(meta.numBytes<=0||meta.data[0]>=0xf8) return;
            // Avoid allocating a MidiMessage for long SysEx messages.
            const auto* d=meta.data; const int n=meta.numBytes;
            if(n<=0) return;
            const int status=d[0], ch=status&15;
            if(status==0xf0 && n>=2) {
                ++sysexCount;
                int len=n-1; if(d[n-1]==0xf7) --len;
                if(len>0) fluid_synth_sysex(s,reinterpret_cast<const char*>(d+1),len,nullptr,nullptr,nullptr,0);
                for(int c=0;c<channels;++c) { updateProgram(s,c); refreshFluidControls(s,c); }
                applyMidiSysEx(*engine,d,n,f,fx);
            } else if(status<0xf0 && status>=0x80 && n>=2) {
                const int a=d[1]&127, v=n>=3 ? d[2]&127 : 0;
                switch(status&0xf0) {
                    case 0x80: if(n>=3) fluid_synth_noteoff(s,ch,a); break;
                    case 0x90: if(n>=3) { if(v) fluid_synth_noteon(s,ch,a,v); else fluid_synth_noteoff(s,ch,a); } break;
                    case 0xa0: if(n>=3) fluid_synth_key_pressure(s,ch,a,v); break;
                    case 0xb0: if(n>=3) { receiveFluidCC(*engine,ch,a,v,f,fx); if(a==121||a==6||a==38||a==96||a==97) refreshFluidControls(s,ch); } break;
                    case 0xc0: receiveProgram(ch,a); fluid_synth_program_change(s,ch,a); updateProgram(s,ch); break;
                    case 0xd0: fluid_synth_channel_pressure(s,ch,a); break;
                    case 0xe0: if(n>=3) fluid_synth_pitch_bend(s,ch,a|(v<<7)); break;
                    default: break;
                }
            }
        };
        auto priority=[](const auto& m) {
            if(m.numBytes<=0) return 4;
            const int st=m.data[0];
            if(st==0xf0) return 0;
            if((st&0xf0)==0xb0 && m.numBytes>=3 && (m.data[1]==0 || m.data[1]==32)) return 1;
            if((st&0xf0)==0xc0) return 2;
            if((st&0xf0)==0x80||(st&0xf0)==0x90) return 4;
            return 3;
        };
        for(auto first=midi.begin(), end=midi.end(); first!=end;) {
            const int time=(*first).samplePosition;
            auto next=first; ++next;
            while(next!=end && (*next).samplePosition==time) ++next;
            bool relevant=false;
            for(auto it=first;it!=next;++it) { const auto m=*it; if(m.numBytes>0 && m.data[0]<0xf8) { relevant=true; break; } }
            if(!relevant) { first=next; continue; }
            const int pos=juce::jlimit(cursor,b.getNumSamples(),time);
            render(cursor,pos-cursor); cursor=pos;
            auto second=first; ++second;
            if(second==next) handleMidi(*first);
            else for(int pass=0;pass<5;++pass)
                for(auto it=first;it!=next;++it) { const auto m=*it; if(priority(m)==pass) handleMidi(m); }
            first=next;
        }
        render(cursor,b.getNumSamples()-cursor);
        const int after=fluid_synth_get_active_voice_count(s); activeVoices.store(after);
        if(after==0&&before==0&&!relevantMidi&&!controlChanged&&rawPeak<0.00001f&&wetPeak<0.00001f)
            engine->quietSamples=juce::jmin(sleepAfter,engine->quietSamples+b.getNumSamples());
        else engine->quietSamples=0;
        midi.clear();
    }
    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::String path; Programs p; Controls c; stateForSave(path,p,&c);
        juce::XmlElement x("LSoundFontState");
        x.setAttribute("version",5); x.setAttribute("midiFX",midiFX->getIndex()); for(int i=0;i<fxCount;++i) x.setAttribute(fxSpecs[i].key,readFXValue(i)); x.setAttribute("sfzQuality",sfzQuality->getIndex()); x.setAttribute("quality",quality->getIndex()); x.setAttribute("effects",effects->getIndex()); x.setAttribute("file",path); x.setAttribute("gain",static_cast<double>(gain->get()));
        for(int i=0;i<channels;++i) x.setAttribute("ch"+juce::String(i+1),p[i]);
        for(int i=0;i<controlCount;++i) if(c[i]!=unset) {
            auto* entry=x.createNewChildElement("Control"); entry->setAttribute("index",i); entry->setAttribute("value",c[i]);
        }
        copyXmlToBinary(x,dest);
    }
    void setStateInformation(const void* data,int size) override {
        if(auto x=getXmlFromBinary(data,size)) if(x->hasTagName("LSoundFontState")) {
            auto p=defaults();
            for(int i=0;i<channels;++i) p[i]=juce::jlimit(0,pack(16383,127),x->getIntAttribute("ch"+juce::String(i+1),p[i]));
            midiFX->setValueNotifyingHost(midiFX->convertTo0to1(float(juce::jlimit(0,1,x->getIntAttribute("midiFX",1)))));
            gain->setValueNotifyingHost(gain->convertTo0to1(static_cast<float>(x->getDoubleAttribute("gain",-6.0))));
            quality->setValueNotifyingHost(quality->convertTo0to1(static_cast<float>(juce::jlimit(0,2,x->getIntAttribute("quality",1)))));
            effects->setValueNotifyingHost(effects->convertTo0to1(static_cast<float>(juce::jlimit(0,3,x->getIntAttribute("effects",0)))));
            sfzQuality->setValueNotifyingHost(sfzQuality->convertTo0to1(static_cast<float>(juce::jlimit(0,11,x->getIntAttribute("sfzQuality",0)))));
            for(int i=0;i<fxCount;++i) { const auto& f=fxSpecs[i]; fxParams[i]->setValueNotifyingHost(fxParams[i]->convertTo0to1(float(juce::jlimit(f.lo,f.hi,x->getIntAttribute(f.key,f.initial))))); }
            auto c=emptyControls();
            for(auto* entry=x->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("Control")) {
                const int i=entry->getIntAttribute("index",-1), v=entry->getIntAttribute("value",unset);
                if(i<0||i>=controlCount||v==unset) continue;
                const int id=i%slots;
                if(id<120&&id!=0&&id!=32) c[i]=juce::jlimit(0,127,v);
                else if(id>=128) if(auto* spec=sf2Spec(id)) c[i]=juce::jlimit(spec->lo,spec->hi,v);
            }
            load(x->getStringAttribute("file"),p,c);
        }
    }
private:
    std::array<juce::RangedAudioParameter*,fxCount> fxParams{};
    std::atomic<int> activeVoices{0}, droppedBlocks{0}, programChanges{0}, lastProgram{-1};
    juce::String lastProgramText() const {
        const int code=lastProgram.load();
        return code<0?juce::String("none"):"CH "+juce::String((code>>7)+1)+", program "+juce::String(code&127);
    }
    void receiveProgram(int ch,int program) {
        lastProgram.store(pack(ch,program)); ++programChanges;
    }
    std::atomic<bool> sleeping{false};
    std::atomic<int> reverbCC{0},chorusCC{0},sysexCount{0},fxSysexCount{0};
    int readFXValue(int i) const { const auto* p=fxParams[i]; return juce::jlimit(fxSpecs[i].lo,fxSpecs[i].hi,juce::roundToInt(p->convertFrom0to1(p->getValue()))); }
    FXValues readFX() const { FXValues f; for(int i=0;i<fxCount;++i) f[i]=readFXValue(i); return f; }
    // MIDI input updates the underlying atomic parameter values without a host
    // automation gesture or a UI callback on the audio thread.
    void midiEffectMask(int next,int& fx) {
        next=juce::jlimit(0,3,next);
        if(next!=fx) { fx=next; static_cast<juce::AudioProcessorParameter*>(effects)->setValue(effects->convertTo0to1(float(fx))); }
    }
    void receiveFluidCC(Engine& e,int ch,int cc,int v,FXValues& f,int& fx) {
        fluid_synth_cc(e.synth,ch,cc,v);
        if(cc<120) controlValues[controlIndex(ch,cc)].store(v);
        if(cc==91||cc==93) {
            if(cc==91) ++reverbCC; else ++chorusCC;
            const int bit=cc==91?1:2;
            if(v>0 && midiFX->getIndex()==1 && !(e.midiDisabledFx&bit) && !(fx&bit)) {
                midiEffectMask(fx|bit,fx); configureFX(e,f,fx);
            }
        }
    }
    void applyMidiSysEx(Engine& e,const std::uint8_t* data,int n,FXValues& f,int& fx) {
        using K=lsfx::MidiFx;
        const bool follow=midiFX->getIndex()==1;
        auto set=[&](int i,int value) {
            value=juce::jlimit(fxSpecs[i].lo,fxSpecs[i].hi,value);
            if(f[i]!=value) { f[i]=value; static_cast<juce::AudioProcessorParameter*>(fxParams[i])->setValue(fxParams[i]->convertTo0to1(float(value))); }
        };
        bool applied=false;
        lsfx::decodeMidiEffects(data,n,[&](K k,int v,int ch,int cc) {
            if(k==K::masterVolume) { e.midiMaster=float(v)/16383.0f; applied=true; return; }
            if(k==K::send) { receiveFluidCC(e,ch,cc,v,f,fx); applied=true; return; }
            if(k==K::reset) { e.midiMaster=1.0f; e.midiDisabledFx=0; }
            if(!follow) return;
            applied=true;
            switch(k) {
                case K::reset:
                    for(int i=0;i<inputTrim;++i) set(i,fxSpecs[i].initial);
                    midiEffectMask(3,fx); break;
                case K::reverbType: set(rvType,v); break;
                case K::chorusType: set(chType,v); break;
                case K::reverbLevel: set(rvLevel,v); break;
                case K::reverbTime: set(rvTime,v); break;
                case K::reverbFeedback: set(delayFeedback,v); break;
                case K::chorusLevel: set(chLevel,v); break;
                case K::chorusRate: set(chRate,v); break;
                case K::chorusDepth: set(chDepth,v); break;
                case K::chorusFeedback: set(chFeedback,v); break;
                case K::delayLevel: set(delayMix,v); break;
                case K::delayTime: set(delayTime,v); break;
                case K::delayFeedback: set(delayFeedback,v); break;
                case K::delayMode: set(delayMode,v); break;
                case K::reverbEnable: case K::chorusEnable: {
                    const int bit=k==K::reverbEnable?1:2;
                    e.midiDisabledFx=v?e.midiDisabledFx&~bit:e.midiDisabledFx|bit;
                    midiEffectMask(v?fx|bit:fx&~bit,fx); break;
                }
                default: break;
            }
        });
        if(applied) { ++fxSysexCount; configureFX(e,f,fx); }
    }
    static bool configureFX(Engine& e,const FXValues& f,int enabled) {
        if(e.lastFx==f&&e.effects==enabled) return false;
        const auto old=e.lastFx;
        if(old[rvType]!=f[rvType]||((e.effects^enabled)&1)) { e.reverbDelay.reset(); e.sfzReverb.reset(); }
        if(old[chType]!=f[chType]||((e.effects^enabled)&2)) e.modulation.reset();
        if(old[delayMode]!=f[delayMode]||(old[delayMix]==0)!=(f[delayMix]==0)) e.delay.reset();
        if(e.synth) {
            auto* s=e.synth;
            if(old[rvType]!=f[rvType]||old[rvLevel]!=f[rvLevel]||old[rvTime]!=f[rvTime]||old[rvDamp]!=f[rvDamp]||old[rvWidth]!=f[rvWidth]||e.effects!=enabled) {
            fluid_synth_reverb_on(s,-1,(enabled&1)&&f[rvType]<6);
            fluid_synth_set_reverb_group_roomsize(s,-1,roomSize(f));
            fluid_synth_set_reverb_group_damp(s,-1,f[rvDamp]*.01);
            fluid_synth_set_reverb_group_width(s,-1,f[rvWidth]*.01);
            fluid_synth_set_reverb_group_level(s,-1,f[rvLevel]*.01);
            }
            if(old[chType]!=f[chType]||old[chLevel]!=f[chLevel]||old[chRate]!=f[chRate]||old[chDepth]!=f[chDepth]||e.effects!=enabled) {
            fluid_synth_chorus_on(s,-1,(enabled&2)&&f[chType]<4);
            fluid_synth_set_chorus_group_nr(s,-1,3+f[chType]%4);
            fluid_synth_set_chorus_group_level(s,-1,f[chLevel]*.01);
            fluid_synth_set_chorus_group_speed(s,-1,f[chRate]*.01);
            fluid_synth_set_chorus_group_depth(s,-1,chorusDepthMs(f));
            }
        } else if(e.sfz && (old[rvType]!=f[rvType]||old[rvLevel]!=f[rvLevel]||old[rvTime]!=f[rvTime]||old[rvDamp]!=f[rvDamp]||old[rvWidth]!=f[rvWidth]||e.effects!=enabled)) {
            juce::Reverb::Parameters p;
            p.roomSize=roomSize(f); p.damping=f[rvDamp]*.01f; p.width=f[rvWidth]*.01f;
            p.wetLevel=f[rvLevel]*.0035f; p.dryLevel=0.5f; p.freezeMode=0;
            e.sfzReverb.setParameters(p);
        }
        e.effects=enabled; e.lastFx=f; return true;
    }
    float processMaster(Engine& e,juce::AudioBuffer<float>& b,const FXValues& f,int enabled,int start=0,int count=-1) {
        const bool reverbEcho=(enabled&1)&&f[rvType]>=6;
        const bool chorusCustom=(enabled&2)&&(e.sfz||f[chType]>=4);
        const bool extraDelay=f[delayMix]>0;
        auto* l=b.getWritePointer(0,start); auto* r=b.getWritePointer(1,start); const int n=count<0?b.getNumSamples()-start:count;
        if(e.sfz&&(enabled&1)&&f[rvType]<6) e.sfzReverb.processStereo(l,r,n);
        const float g=juce::Decibels::decibelsToGain(gain->get())*e.midiMaster, trim=juce::Decibels::decibelsToGain(float(f[inputTrim]));
        float peak=0;
        for(int i=0;i<n;++i) {
            const float dryL=std::isfinite(l[i])?l[i]:0, dryR=std::isfinite(r[i])?r[i]:0; l[i]=dryL; r[i]=dryR; float wl=0,wr=0;
            if(reverbEcho) {
                e.reverbDelay.process(dryL,dryR,float(f[delayTime]),0,0,f[delayFeedback]*.01f,f[rvType]==7,wl,wr);
                l[i]+=wl*f[rvLevel]*.01f; r[i]+=wr*f[rvLevel]*.01f;
            }
            if(chorusCustom) {
                const int t=f[chType]; const float ms=t==5?3.0f:t>=6?35.0f:14.0f;
                const float fb=t==4||t==5||t==7?f[chFeedback]*.01f:0;
                e.modulation.process(dryL,dryR,ms,chorusDepthMs(f),f[chRate]*.01f,fb,false,wl,wr);
                l[i]+=wl*f[chLevel]*.01f; r[i]+=wr*f[chLevel]*.01f;
            }
            if(extraDelay) {
                e.delay.process(dryL,dryR,float(f[delayTime]),0,0,f[delayFeedback]*.01f,f[delayMode]!=0,wl,wr);
                l[i]+=wl*f[delayMix]*.01f; r[i]+=wr*f[delayMix]*.01f;
            }
            peak=(std::max)(peak,(std::max)(std::abs(l[i]),std::abs(r[i])));
            e.output.process(l[i],r[i],g,trim);
        }
        return peak;
    }
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterChoice* quality = nullptr;
    juce::AudioParameterChoice* effects = nullptr;
    juce::AudioParameterChoice* midiFX = nullptr;
    juce::AudioParameterChoice* sfzQuality = nullptr;
    std::array<std::atomic<int>,controlCount> controlValues{}, controlCommands{};
    std::atomic<bool> controlDirty{false};
    Controls overrides=emptyControls(); // protected by requestMutex
    static void sendControl(Engine& e,int ch,int id,int value) {
        if(e.sfz) { if(ch==0&&id<120&&id!=0&&id!=32) sfizz_send_cc(e.sfz,0,id,value); }
        else if(e.synth) {
            if(id<120&&id!=0&&id!=32) fluid_synth_cc(e.synth,ch,id,value);
            else if(id>=128) if(auto* spec=sf2Spec(id)) fluid_synth_set_gen(e.synth,ch,spec->gen,static_cast<float>(value));
        }
    }
    bool applyControls(Engine& e) {
        if(!controlDirty.exchange(false)) return false;
        for(int i=0;i<controlCount;++i) {
            const int v=controlCommands[i].exchange(unset);
            if(v!=unset) { sendControl(e,i/slots,i%slots,v); controlValues[i].store(v); }
        }
        return true;
    }
    void refreshFluidControls(fluid_synth_t* synth,int ch) {
        for(const auto& spec:sf2Controls) {
            int v=0;
            if(spec.gen<0) fluid_synth_get_cc(synth,ch,spec.id,&v);
            else v=static_cast<int>(std::round(fluid_synth_get_gen(synth,ch,spec.gen)));
            controlValues[controlIndex(ch,spec.id)].store(v);
        }
    }
    std::atomic<double> hostRate{44100};
    std::array<std::atomic<int>,channels> selected{},commands{};
    std::atomic<bool> panic{false};
    std::mutex engineMutex, requestMutex, viewMutex;
    std::condition_variable cv;
    std::unique_ptr<Engine> engine;
    std::thread worker;
    bool stopping=false, pending=false;
    Request latest;
    View view;
    static void choose(Engine& e,int ch,int code) {
        fluid_synth_program_select(e.synth,ch,e.font,code>>7,code&127);
    }
    void updateProgram(fluid_synth_t* s,int ch) {
        int font=-1,bank=0,prog=0;
        if(fluid_synth_get_program(s,ch,&font,&bank,&prog)==FLUID_OK && bank>=0 && bank<=16383 && prog>=0 && prog<128) selected[ch].store(pack(bank,prog));
    }
    void stateForSave(juce::String& path, Programs& p, Controls* controls=nullptr) {
        std::lock_guard<std::mutex> lock(requestMutex);
        path=latest.path;
        if(controls) *controls=(pending||loadingSerial!=0||failedSerial==latest.serial)?latest.controls:overrides;
        // While a new bank is loading retain the requested state, not old programs.
        if (pending || loadingSerial!=0 || failedSerial==latest.serial) p=latest.programs;
        else for(int i=0;i<channels;++i) { int cmd=commands[i].load(); p[i]=cmd>=0 ? cmd : selected[i].load(); }
    }
    uint64_t loadingSerial=0, failedSerial=~uint64_t(0);
    void load(const juce::String& path,Programs p,Controls c=emptyControls()) {
        { std::lock_guard<std::mutex> lock(requestMutex);
          latest={path,hostRate.load(),p,latest.serial+1,c}; pending=true;
          std::lock_guard<std::mutex> ui(viewMutex); view.loading=true; view.status=path.isEmpty()?"Unloading SoundFont":"Loading "+juce::File(path).getFileName(); ++view.revision;
        }
        cv.notify_one();
    }
    void work() {
        for(;;) {
            Request r;
            { std::unique_lock<std::mutex> lock(requestMutex); cv.wait(lock,[&]{return stopping||pending;}); if(stopping) return; r=latest; pending=false; loadingSerial=r.serial; }
            std::unique_ptr<Engine> fresh;
            std::vector<Preset> presets;
            std::vector<CCLabel> labels;
            juce::String error;
            if(r.path.isNotEmpty()) {
                if(!juce::File(r.path).existsAsFile()) error="SoundFont not found: "+r.path;
                else if(juce::File(r.path).hasFileExtension("sfz")) {
                    fresh=std::make_unique<Engine>(); fresh->rate=r.rate; fresh->prepareFX();
                    fresh->sfz=sfizz_create_synth();
                    if(fresh->sfz) {
                        sfizz_set_sample_rate(fresh->sfz,static_cast<float>(r.rate));
                        sfizz_set_samples_per_block(fresh->sfz,1024);
                        sfizz_set_num_voices(fresh->sfz,128);
                    }
                    if(!fresh->sfz || !sfizz_load_file(fresh->sfz,r.path.toRawUTF8())) error="Unable to load SFZ: "+r.path;
                    else {
                        fresh->sfzDefaultQuality=sfizz_get_sample_quality(fresh->sfz,SFIZZ_PROCESS_LIVE);
                        const auto n=sfizz_get_num_cc_labels(fresh->sfz);
                        for(unsigned i=0;i<n;++i) {
                            const int cc=sfizz_get_cc_label_number(fresh->sfz,static_cast<int>(i));
                            if(cc<=0||cc==32||cc>=120) continue;
                            const char* text=sfizz_get_cc_label_text(fresh->sfz,static_cast<int>(i));
                            if(text&&*text) labels.push_back({cc,juce::String::fromUTF8(text)});
                        }
                        std::sort(labels.begin(),labels.end(),[](const CCLabel& a,const CCLabel& b){return a.cc<b.cc;});
                        labels.erase(std::unique(labels.begin(),labels.end(),[](const CCLabel& a,const CCLabel& b){return a.cc==b.cc;}),labels.end());
                        // sfizz 1.2.3 dispatches these read-only OSC replies synchronously.
                        // Query the fresh, unpublished synth; no concurrent audio calls.
                        int reply=unset;
                        if(auto* client=sfizz_create_client(&reply)) {
                            sfizz_set_receive_callback(client,[](void* data,int,const char*,const char* sig,const sfizz_arg_t* args){
                                if(sig&&sig[0]=='f'&&args&&std::isfinite(args[0].f))
                                    *static_cast<int*>(data)=juce::jlimit(0,127,juce::roundToInt(args[0].f*127.0f));
                            });
                            for(auto& cc:labels) {
                                reply=unset;
                                const auto path="/cc"+juce::String(cc.cc)+"/value";
                                sfizz_send_message(fresh->sfz,client,0,path.toRawUTF8(),"",nullptr);
                                cc.initial=reply;
                            }
                            sfizz_delete_client(client);
                        }
                    }
                } else {
                    fresh=std::make_unique<Engine>(); fresh->rate=r.rate; fresh->prepareFX();
                    fresh->settings=new_fluid_settings();
                    if(fresh->settings) {
                        fluid_settings_setnum(fresh->settings,"synth.sample-rate",r.rate);
                        fluid_settings_setint(fresh->settings,"synth.midi-channels",16);
                        fluid_settings_setint(fresh->settings,"synth.polyphony",256);
                        fluid_settings_setint(fresh->settings,"synth.reverb.active",0);
                        fluid_settings_setint(fresh->settings,"synth.chorus.active",0);
                        fluid_settings_setstr(fresh->settings,"synth.midi-bank-select","gs");
                        fluid_settings_setint(fresh->settings,"synth.threadsafe-api",0);
                        fluid_settings_setint(fresh->settings,"synth.cpu-cores",1);
                        fresh->synth=new_fluid_synth(fresh->settings);
                    }
                    if(fresh->synth) fresh->font=fluid_synth_sfload(fresh->synth,r.path.toRawUTF8(),1);
                    if(fresh->font<0) error="Unable to load SoundFont: "+r.path;
                    else {
                        fluid_synth_set_interp_method(fresh->synth,-1,FLUID_INTERP_4THORDER);
                        if(auto* sf=fluid_synth_get_sfont_by_id(fresh->synth,fresh->font)) {
                            fluid_sfont_iteration_start(sf);
                            while(auto* p=fluid_sfont_iteration_next(sf)) {
                                const int bank=fluid_preset_get_banknum(p), program=fluid_preset_get_num(p);
                                if(bank>=0 && bank<=16383 && program>=0 && program<128) presets.push_back({bank,program,juce::String::fromUTF8(fluid_preset_get_name(p))});
                            }
                        }
                        std::sort(presets.begin(),presets.end(),[](const Preset& a,const Preset& b){return pack(a.bank,a.program)<pack(b.bank,b.program);});
                        for(int i=0;i<channels;++i) choose(*fresh,i,r.programs[i]);
                    }
                }
            }
            std::unique_ptr<Engine> retired;
            { std::lock_guard<std::mutex> lock(requestMutex);
              if(stopping) return;
              if(r.serial!=latest.serial) { loadingSerial=0; continue; }
              // Failed replacement unloads the previous bank deliberately: no wrong-bank playback.
              { std::lock_guard<std::mutex> audio(engineMutex);
                retired=std::move(engine);
                if(error.isEmpty()) engine=std::move(fresh);
                overrides=r.controls; controlDirty.store(false);
                for(int i=0;i<controlCount;++i) { controlValues[i].store(unset); controlCommands[i].store(unset); }
                if(engine) {
                    // Restore explicit overrides on the first audio callback, not the loader thread.
                    if(engine->synth) for(int ch=0;ch<channels;++ch) refreshFluidControls(engine->synth,ch);
                    else if(engine->sfz) for(const auto& cc:labels) controlValues[controlIndex(0,cc.cc)].store(cc.initial);
                    for(int i=0;i<controlCount;++i) if(r.controls[i]!=unset && (engine->synth || (i<120))) {
                        controlCommands[i].store(r.controls[i]); controlDirty.store(true);
                    }
                }
                for(int i=0;i<channels;++i) { commands[i].store(-1); selected[i].store(r.programs[i]); if(engine && engine->synth) updateProgram(engine->synth,i); }
              }
              loadingSerial=0; failedSerial=error.isNotEmpty()?r.serial:~uint64_t(0);
              { std::lock_guard<std::mutex> ui(viewMutex);
                view.isSfz=error.isEmpty() && juce::File(r.path).hasFileExtension("sfz"); view.ready=error.isEmpty()&&r.path.isNotEmpty(); view.labels=std::move(labels); view.path=r.path; view.presets=error.isEmpty()?std::move(presets):std::vector<Preset>{}; view.loading=false;
                view.status=error.isNotEmpty()?error:r.path.isEmpty()?"No SoundFont loaded":"Loaded: "+juce::File(r.path).getFileName()+"; "+(view.isSfz?juce::String("SFZ, MIDI channel 1"):juce::String(static_cast<int>(view.presets.size()))+" presets");
                ++view.revision;
              }
            }
            // Destroy old synth and samples here, outside the audio lock and audio thread.
        }
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

// The grid follows LJuno's two-focus model: browse parameter names/values,
// then Enter into the value editor. All DSP calls remain on the audio thread.
constexpr int stepWidths[]={1,5,10,15,20};
constexpr int pageMultiplier=40;
constexpr int gridRowsPerColumn=8, gridColumnsPerPage=2, gridPageRows=gridRowsPerColumn*gridColumnsPerPage;
constexpr int rowGain=-1,rowInterpolation=-2,rowEffects=-3,rowSfzQuality=-4,rowChannel=-5,rowMidiFX=-6;
struct GridRow { int id; juce::String name; double low,high,step; bool choice; juce::String unit; };

template<class C> class Keyed : public C {
public:
    using C::C;
    std::function<bool(const juce::KeyPress&,juce::Component*)> keyHandler;
    bool keyPressed(const juce::KeyPress& k) override {
        if(keyHandler&&keyHandler(k,this)) return true;
        return C::keyPressed(k);
    }
};

// Based on the accessibility notification technique used in LJuno-116.
// Windows uses the focused control's own UIA provider, rather than a speech DLL.
static void speak(juce::Component& source,const juce::String& message) {
    if(!source.hasKeyboardFocus(true)) return;
#if JUCE_WINDOWS
    if(!UiaClientsAreListening()) return;
    if(auto* handler=source.getAccessibilityHandler()) {
        auto* native=reinterpret_cast<IUnknown*>(handler->getNativeImplementation());
        IRawElementProviderSimple* provider=nullptr;
        if(native&&SUCCEEDED(native->QueryInterface(__uuidof(IRawElementProviderSimple),reinterpret_cast<void**>(&provider)))) {
            auto* text=SysAllocString(message.toWideCharPointer());
            auto* activity=SysAllocString(L"LSFX");
            UiaRaiseNotificationEvent(provider,NotificationKind_Other,NotificationProcessing_ImportantMostRecent,text,activity);
            SysFreeString(text); SysFreeString(activity); provider->Release(); return;
        }
    }
#endif
    juce::ignoreUnused(source);
    juce::AccessibilityHandler::postAnnouncement(message,juce::AccessibilityHandler::AnnouncementPriority::high);
}

class Editor final : public juce::AudioProcessorEditor, private juce::Timer, private juce::KeyListener {
public:
    explicit Editor(Processor& p) : AudioProcessorEditor(p), proc(p) {
        setWantsKeyboardFocus(true);
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        title.setText("LSFX 0.0.1 TEST11",juce::dontSendNotification); title.setFont(juce::FontOptions(21.0f)); title.setAccessible(false); addAndMakeVisible(title);
        status.setText("No SoundFont loaded",juce::dontSendNotification); status.setTitle("Instrument status"); addAndMakeVisible(status);
        setup(open,"Open SF2 or SFZ",1); setup(parameters,"Parameters",2); setup(channel,"MIDI channel",3); setup(bank,"Bank",4); setup(program,"Program",5); setup(level,"Output gain",6); setup(stop,"Panic - all sounds off",7); setup(readStatus,"Read status",8);
        setup(grid,"Parameter grid",1); setup(value,"Parameter value",2); setup(closeGrid,"Close parameters",3);
        open.setDescription("Alt+O"); parameters.setDescription("Alt+L"); channel.setDescription("Alt+M"); bank.setDescription("Alt+B"); program.setDescription("Alt+P"); level.setDescription("Alt+G"); stop.setDescription("Alt+A"); readStatus.setDescription("Alt+S");
        grid.setDescription("Alt+L. Enter or Tab edits the selected parameter. Escape closes parameters.");
        channelLabel.setText("MIDI channel",juce::dontSendNotification); bankLabel.setText("Bank",juce::dontSendNotification); programLabel.setText("Program",juce::dontSendNotification); levelLabel.setText("Output gain (dB)",juce::dontSendNotification);
        gridTitle.setText("Parameters",juce::dontSendNotification); hint.setText("Arrows: grid or value. Type to search. Enter: edit / browse. Esc: close.",juce::dontSendNotification);
        for(auto* l:{&channelLabel,&bankLabel,&programLabel,&levelLabel,&gridTitle,&hint}) addAndMakeVisible(*l);
        for(int i=1;i<=16;++i) channel.addItem("Channel "+juce::String(i),i);
        channel.setSelectedId(1,juce::dontSendNotification);
        channel.onChange=[this]{ shownCode=-1; refresh(true); if(gridOpen) rebuildGrid(); };
        bank.onChange=[this]{ if(!updating) { rebuildPrograms(bank.getSelectedId()-1,-1); if(program.getNumItems()>0) { program.setSelectedItemIndex(0,juce::dontSendNotification); apply(); } } };
        program.onChange=[this]{ if(!updating) apply(); };
        open.onClick=[this]{ browse(); }; parameters.onClick=[this]{ openParameters(); };
        stop.onClick=[this]{proc.allOff();};
        readStatus.onClick=[this]{ juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"LSFX status",proc.getView().status+"\n"+proc.diagnostic()); };
        closeGrid.onClick=[this]{ closeParameters(); };
        level.setSliderStyle(juce::Slider::LinearHorizontal); level.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0); level.textFromValueFunction=[](double v){return juce::String(v,1)+" dB";}; level.setRange(-60.0,6.0,0.1); level.setValue(proc.gainParameter().get(),juce::dontSendNotification);
        level.onDragStart=[this]{proc.gainParameter().beginChangeGesture(); dragging=true;};
        level.onDragEnd=[this]{dragging=false;proc.gainParameter().endChangeGesture();};
        level.onValueChange=[this]{ auto& a=proc.gainParameter(); if(!dragging) a.beginChangeGesture(); a.setValueNotifyingHost(a.convertTo0to1(static_cast<float>(level.getValue()))); if(!dragging) a.endChangeGesture(); levelReadout.setText(juce::String(level.getValue(),1)+" dB",juce::dontSendNotification); };
        value.setSliderStyle(juce::Slider::LinearHorizontal); value.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0);
        for(auto* l:{&levelReadout,&valueReadout}) {l->setJustificationType(juce::Justification::centredRight); l->setAccessible(false); addAndMakeVisible(*l);}
        grid.onChange=[this]{ if(!updatingGrid) {search.reset(); syncValue(); repaint();} };
        value.onValueChange=[this]{ if(!updatingGrid) { if(auto* row=selectedRow()) { const auto r=*row; writeRow(r,value.getValue()); refreshGridValues(); } } };
        // Keyed controls intercept their own arrows before JUCE's native behavior;
        // the editor listener also handles Alt accelerators bubbling from children.
        addKeyListener(this);
        showGrid(false); setSize(760,570); refresh(true); startTimerHz(5);
        scheduleInitialFocus();
    }
    ~Editor() override { stopTimer(); removeKeyListener(this); chooser.reset(); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff202429));
        if(!gridOpen) return;
        const auto count=static_cast<int>(rows.size());
        const int current=juce::jmax(0,grid.getSelectedItemIndex());
        const int first=(current/gridPageRows)*gridPageRows;
        g.setColour(juce::Colour(0xffaeb9c6));
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText("Grid  -  page "+juce::String(first/gridPageRows+1)+" / "+juce::String(juce::jmax(1,(count+gridPageRows-1)/gridPageRows)),gridPageBounds,juce::Justification::centredLeft,true);
        for(int k=0;k<gridPageRows;++k) {
            const int index=first+k; const auto cell=gridCell(k);
            const bool selected=index==current&&index<count;
            g.setColour(selected?juce::Colour(0xff304d66):juce::Colour(0xff272e35)); g.fillRect(cell);
            g.setColour(selected?juce::Colour(0xffb0d6ff):juce::Colour(0xff657481)); g.drawRect(cell,selected?2:1);
            if(index>=count) continue;
            const auto& row=rows[static_cast<size_t>(index)];
            auto content=cell.reduced(7,1); auto number=content.removeFromLeft(26);
            g.setColour(juce::Colour(0xffafbac5)); g.drawText(juce::String(index+1),number,juce::Justification::centredLeft,true);
            auto right=content.removeFromRight(116); content.removeFromRight(4);
            g.setColour(juce::Colours::white); g.drawFittedText(row.name,content,juce::Justification::centredLeft,1);
            g.setColour(juce::Colour(0xffd0e4f7)); g.drawFittedText(formatValue(row,readRow(row)),right,juce::Justification::centredRight,1);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if(!gridOpen||!gridArea.contains(e.getPosition())) return;
        const int first=(juce::jmax(0,grid.getSelectedItemIndex())/gridPageRows)*gridPageRows;
        for(int k=0;k<gridPageRows;++k) if(gridCell(k).contains(e.getPosition())&&first+k<static_cast<int>(rows.size())) {
            search.reset(); selectGridRow(first+k,false); focus(grid); break;
        }
    }
    void focusGained(FocusChangeType) override { initialFocusPending=true; scheduleInitialFocus(); }
    void visibilityChanged() override { if(isVisible()) { initialFocusPending=true; scheduleInitialFocus(); } }
    void parentHierarchyChanged() override { if(isShowing()) scheduleInitialFocus(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return createIgnoredAccessibilityHandler(*this);
    }
    void resized() override {
        auto r=getLocalBounds().reduced(16); title.setBounds(r.removeFromTop(30)); status.setBounds(r.removeFromTop(30));
        if(gridOpen) {
            r.removeFromTop(8); gridTitle.setBounds(r.removeFromTop(28)); grid.setBounds(r.removeFromTop(34)); r.removeFromTop(6);
            auto v=r.removeFromTop(38); valueReadout.setBounds(v.removeFromRight(180)); value.setBounds(v.reduced(0,2));
            r.removeFromTop(6); gridPageBounds=r.removeFromTop(20); gridArea=r.removeFromTop(240);
            r.removeFromTop(6); hint.setBounds(r.removeFromTop(30)); closeGrid.setBounds(r.removeFromTop(34)); return;
        }
        open.setBounds(r.removeFromTop(30)); r.removeFromTop(4); parameters.setBounds(r.removeFromTop(30)); r.removeFromTop(8);
        auto line=[&](juce::Label& l,juce::Component& c){auto a=r.removeFromTop(38);l.setBounds(a.removeFromLeft(150));c.setBounds(a.reduced(0,3));};
        line(channelLabel,channel);line(bankLabel,bank);line(programLabel,program);
        auto lv=r.removeFromTop(38); levelLabel.setBounds(lv.removeFromLeft(150)); levelReadout.setBounds(lv.removeFromRight(100)); level.setBounds(lv.reduced(0,3)); r.removeFromTop(6);auto a=r.removeFromTop(32);stop.setBounds(a.removeFromLeft(a.getWidth()/2).reduced(2));readStatus.setBounds(a.reduced(2));
    }
private:
    Processor& proc;
    juce::Label title,status,channelLabel,bankLabel,programLabel,levelLabel,gridTitle,hint,levelReadout,valueReadout;
    juce::Rectangle<int> gridArea,gridPageBounds;
    ljuno::IncrementalParameterSearch search;
    Keyed<juce::TextButton> open{"Open SF2 or SFZ"},parameters{"Parameters"},stop{"Panic - all sounds off"},readStatus{"Read status"},closeGrid{"Close parameters"};
    Keyed<juce::ComboBox> channel,bank,program,grid;
    Keyed<juce::Slider> level,value;
    std::unique_ptr<juce::FileChooser> chooser;
    View cached;
    std::vector<GridRow> rows;
    uint64_t revision=~uint64_t(0);
    int shownCode=-1,stepIndex=0,lastSf2Row=rowGain,lastSfzRow=rowGain;
    bool updating=false,dragging=false,gridOpen=false,updatingGrid=false,initialFocusPending=true,focusScheduled=false;
    template<class C> void setup(Keyed<C>& c,const juce::String& name,int order) {
        c.setTitle(name); c.setWantsKeyboardFocus(true); c.setExplicitFocusOrder(order);
        c.keyHandler=[this](const juce::KeyPress& k,juce::Component* origin){return handleKey(k,origin);}; addAndMakeVisible(c);
    }
    juce::Rectangle<int> gridCell(int k) const {
        const int col=k/gridRowsPerColumn, row=k%gridRowsPerColumn;
        const int w=gridArea.getWidth()/gridColumnsPerPage, h=gridArea.getHeight()/gridRowsPerColumn;
        return {gridArea.getX()+col*w,gridArea.getY()+row*h,w,h};
    }
    // Do not publish a UIA focus event unless the native keyboard focus really arrived.
    // Never take focus back from REAPER's FX list or another child on a timer.
    void focus(juce::Component& c) {
        if(!c.isShowing()||!c.isEnabled()) return;
        if(!c.hasKeyboardFocus(true)) {
            juce::AccessibilityHandler::clearCurrentlyFocusedHandler(); c.grabKeyboardFocus();
            if(c.hasKeyboardFocus(true)) if(auto* handler=c.getAccessibilityHandler()) handler->grabFocus();
        }
    }
    void scheduleInitialFocus() {
        if(!initialFocusPending||focusScheduled||!isShowing()) return;
        focusScheduled=true;
        juce::Component::SafePointer<Editor> safe(this);
        juce::Timer::callAfterDelay(150,[safe]{if(auto* e=safe.getComponent()) {
            e->focusScheduled=false;
            if(!e->initialFocusPending||!e->isShowing()||e->chooser) return;
            auto* focused=juce::Component::getCurrentlyFocusedComponent();
            if(focused!=nullptr&&focused!=e) {
                if(e->isParentOf(focused)) e->initialFocusPending=false;
                return;
            }
            auto* peer=e->getPeer();
            if(peer==nullptr||!peer->isFocused()) return;
            auto& target=e->gridOpen?static_cast<juce::Component&>(e->grid):static_cast<juce::Component&>(e->open);
            auto* h=target.getAccessibilityHandler();
#if JUCE_WINDOWS
            if(h==nullptr||h->getNativeImplementation()==nullptr) return;
#else
            juce::ignoreUnused(h);
#endif
            e->initialFocusPending=false; e->focus(target);
            if(!target.hasKeyboardFocus(true)) e->initialFocusPending=true;
        }});
    }
    void showGrid(bool show,juce::Component* destination=nullptr) {
        gridOpen=show; search.reset();
        const auto main=std::array<juce::Component*,13>{&open,&parameters,&channel,&bank,&program,&level,&stop,&readStatus,&channelLabel,&bankLabel,&programLabel,&levelLabel,&levelReadout};
        const auto panel=std::array<juce::Component*,6>{&grid,&value,&closeGrid,&gridTitle,&hint,&valueReadout};
        // Reveal the destination before hiding the old focused controls.
        if(show) for(auto* c:panel) c->setVisible(true);
        else for(auto* c:main) c->setVisible(true);
        resized();
        if(show) { if(hasKeyboardFocus(true)) focus(grid); for(auto* c:main) c->setVisible(false); }
        else { if(grid.hasKeyboardFocus(true)||value.hasKeyboardFocus(true)||closeGrid.hasKeyboardFocus(true)) focus(destination!=nullptr?*destination:static_cast<juce::Component&>(parameters)); for(auto* c:panel) c->setVisible(false); }
        repaint();
    }
    void openParameters() {
        if(!gridOpen) { refresh(false); rebuildGrid(); showGrid(true); }
        focus(grid);
    }
    void closeParameters() { if(!gridOpen) return; showGrid(false); focus(parameters); }
    int selectedChannel() const { return cached.isSfz?0:juce::jlimit(0,15,channel.getSelectedId()-1); }
    const GridRow* selectedRow() const { const int i=grid.getSelectedItemIndex(); return i>=0&&i<static_cast<int>(rows.size())?&rows[static_cast<size_t>(i)]:nullptr; }
    void rebuildGrid() {
        const int preferred=cached.isSfz?lastSfzRow:lastSf2Row;
        rows.clear();
        rows.push_back({rowGain,"Output gain",-60,6,0.1,false," dB"});
        if(cached.ready&&!cached.loading) {
            if(cached.isSfz) {
                rows.push_back({rowSfzQuality,"SFZ live sample quality",0,11,1,true,{}});
                for(const auto& cc:cached.labels) rows.push_back({cc.cc,cc.name+" (CC"+juce::String(cc.cc)+")",0,127,1,false,{}});
            } else {
                rows.push_back({rowChannel,"MIDI channel",1,16,1,true,{}});
                rows.push_back({rowInterpolation,"SF2 interpolation",0,2,1,true,{}});
                
                for(const auto& c:sf2Controls) rows.push_back({c.id,c.name,static_cast<double>(c.lo),static_cast<double>(c.hi),static_cast<double>(c.step),false,c.unit});
            }
        }
        if(cached.ready&&!cached.loading) {
            rows.push_back({rowEffects,"Effects",0,3,1,true,{}});
            if(!cached.isSfz) rows.push_back({rowMidiFX,"MIDI effects reception",0,1,1,true,{}});
            for(int i=0;i<fxCount;++i) { const auto& f=fxSpecs[i]; rows.push_back({fxRow(i),f.label,double(f.lo),double(f.hi),1,i==rvType||i==chType||i==delayMode,f.unit}); }
        }
        gridTitle.setText(cached.ready&&!cached.loading?(cached.isSfz?"SFZ parameters":"SF2 parameters - channel "+juce::String(selectedChannel()+1)):"Parameters",juce::dontSendNotification);
        updatingGrid=true; grid.clear(juce::dontSendNotification);
        int target=0;
        for(size_t i=0;i<rows.size();++i) { if(rows[i].id==preferred) target=static_cast<int>(i); grid.addItem(rowLine(rows[i]),static_cast<int>(i)+1); }
        grid.setSelectedItemIndex(target,juce::dontSendNotification); updatingGrid=false; search.reset(); syncValue(); repaint();
    }
    double readRow(const GridRow& r) const {
        if(const int i=fxIndex(r.id);i>=0) { auto& p=proc.fxParameter(i); return p.convertFrom0to1(p.getValue()); }
        if(r.id==rowGain) return proc.gainParameter().get();
        if(r.id==rowInterpolation) return proc.qualityParameter().getIndex();
        if(r.id==rowEffects) return proc.effectsParameter().getIndex();
        if(r.id==rowMidiFX) return proc.midiFXParameter().getIndex();
        if(r.id==rowSfzQuality) return proc.sfzQualityParameter().getIndex();
        if(r.id==rowChannel) return selectedChannel()+1;
        return proc.getControl(selectedChannel(),r.id);
    }
    juce::String formatValue(const GridRow& r,double v) const {
        if(v==unset) return "Instrument default (not read)";
        if(const int i=fxIndex(r.id);i>=0) {
            const int k=juce::jlimit(fxSpecs[i].lo,fxSpecs[i].hi,juce::roundToInt(v));
            if(i==rvType) return reverbNames[k]; if(i==chType) return chorusNames[k]; if(i==delayMode) return k?"Ping-pong":"Stereo";
            if(i==chRate) return juce::String(k*.01,2)+" Hz";
        }
        if(r.id==rowGain) return juce::String(v,1)+r.unit;
        if(r.id==rowInterpolation) return proc.qualityParameter().choices[juce::jlimit(0,2,juce::roundToInt(v))];
        if(r.id==rowMidiFX) return proc.midiFXParameter().choices[juce::jlimit(0,1,juce::roundToInt(v))];
        if(r.id==rowEffects) return proc.effectsParameter().choices[juce::jlimit(0,3,juce::roundToInt(v))];
        if(r.id==rowSfzQuality) return proc.sfzQualityParameter().choices[juce::jlimit(0,11,juce::roundToInt(v))];
        return juce::String(juce::roundToInt(v))+r.unit;
    }
    juce::String rowLine(const GridRow& r) const { return r.name+", "+formatValue(r,readRow(r)); }
    void writeRow(const GridRow& r,double v) {
        v=juce::jlimit(r.low,r.high,r.low+std::round((v-r.low)/r.step)*r.step);
        auto set=[v](juce::RangedAudioParameter& p){ p.beginChangeGesture(); p.setValueNotifyingHost(p.convertTo0to1(static_cast<float>(v))); p.endChangeGesture(); };
        if(const int i=fxIndex(r.id);i>=0) set(proc.fxParameter(i));
        else if(r.id==rowGain) set(proc.gainParameter());
        else if(r.id==rowInterpolation) set(proc.qualityParameter());
        else if(r.id==rowEffects) set(proc.effectsParameter());
        else if(r.id==rowMidiFX) set(proc.midiFXParameter());
        else if(r.id==rowSfzQuality) set(proc.sfzQualityParameter());
        else if(r.id==rowChannel) { channel.setSelectedId(juce::roundToInt(v),juce::dontSendNotification); shownCode=-1; refresh(true); gridTitle.setText("SF2 parameters - channel "+juce::String(selectedChannel()+1),juce::dontSendNotification); }
        else proc.setControl(selectedChannel(),r.id,juce::roundToInt(v));
    }
    void syncValue() {
        if(auto* row=selectedRow()) {
            const auto r=*row; if(cached.isSfz) lastSfzRow=r.id; else lastSf2Row=r.id;
            updatingGrid=true;
            value.setTitle(r.name); value.setName(r.name); value.setRange(r.low,r.high,r.step);
            value.textFromValueFunction=[this,r](double v) { return formatValue(r,(r.id>=0&&readRow(r)==unset&&v==r.low)?static_cast<double>(unset):v); };
            const auto v=readRow(r); value.setValue(v==unset?r.low:v,juce::dontSendNotification); value.updateText(); valueReadout.setText(formatValue(r,v),juce::dontSendNotification);
            updatingGrid=false;
        }
    }
    void refreshGridValues() {
        if(!gridOpen||grid.isPopupActive()) return;
        const int selectedId=grid.getSelectedId();
        bool changed=false; updatingGrid=true;
        for(size_t i=0;i<rows.size();++i) { const auto text=rowLine(rows[i]); if(grid.getItemText(static_cast<int>(i))!=text) {grid.changeItemText(static_cast<int>(i)+1,text); changed=true;} }
        if(selectedId>0 && grid.getSelectedId()!=selectedId) grid.setSelectedId(selectedId,juce::dontSendNotification);
        if(auto* r=selectedRow()) {
            const double v=readRow(*r); const auto text=formatValue(*r,v);
            if(!value.isMouseButtonDown()) {
                const double actual=v==unset?r->low:v;
                if(std::abs(value.getValue()-actual)>0.000001) value.setValue(actual,juce::dontSendNotification);
                if(valueReadout.getText()!=text) {value.updateText(); valueReadout.setText(text,juce::dontSendNotification); changed=true;}
            }
        }
        updatingGrid=false; if(changed) repaint();
    }
    void selectGridRow(int target,bool announce) {
        if(rows.empty()) return;
        target=juce::jlimit(0,static_cast<int>(rows.size())-1,target);
        grid.setSelectedItemIndex(target,juce::dontSendNotification); syncValue(); repaint();
        if(announce) speak(grid,rowLine(rows[static_cast<size_t>(target)]));
    }
    void editSelected(juce::Component& source,int direction,bool coarse,bool boundary=false) {
        if(auto* row=selectedRow()) {
            const auto r=*row; double v=readRow(r); if(v==unset) v=r.low;
            const double delta=r.step*(r.choice?1:stepWidths[stepIndex]*(coarse?pageMultiplier:1))*direction;
            writeRow(r,boundary?(direction>0?r.high:r.low):v+delta); refreshGridValues();
            speak(source,formatValue(r,readRow(r)));
        }
    }
    void changeStep(juce::Component& source,int d) { stepIndex=juce::jlimit(0,4,stepIndex+d); speak(source,"Step "+juce::String(stepWidths[stepIndex])); }
    bool keyPressed(const juce::KeyPress& key,juce::Component* origin) override { return handleKey(key,origin); }
    juce::Component* keyboardControl(juce::Component* origin) {
        // Some JUCE controls forward a key from their internal label, rather
        // than the slider/combobox itself. Treat both routes identically.
        for(auto* c=origin;c!=nullptr&&c!=this;c=c->getParentComponent()) {
            for(auto* target:std::array<juce::Component*,11>{&grid,&value,&level,&open,&parameters,&channel,&bank,&program,&stop,&readStatus,&closeGrid}) if(c==target) return target;
        }
        return origin;
    }
    void changeMainLevel(int direction,bool coarse,bool boundary=false) {
        const double next=boundary?(direction>0?level.getMaximum():level.getMinimum()):level.getValue()+0.1*stepWidths[stepIndex]*(coarse?pageMultiplier:1)*direction;
        level.setValue(juce::jlimit(level.getMinimum(),level.getMaximum(),next),juce::sendNotificationSync);
        levelReadout.setText(juce::String(level.getValue(),1)+" dB",juce::dontSendNotification);
        speak(level,levelReadout.getText());
    }
    void navigateGrid(int code) {
        if(rows.empty()) return;
        const int i=juce::jmax(0,grid.getSelectedItemIndex()), count=static_cast<int>(rows.size());
        const int row=i%gridRowsPerColumn, first=i-row, last=juce::jmin(first+gridRowsPerColumn-1,count-1);
        int target=i;
        if(code==juce::KeyPress::upKey) target=juce::jmax(first,i-1);
        else if(code==juce::KeyPress::downKey) target=juce::jmin(last,i+1);
        else if(code==juce::KeyPress::leftKey&&first>0) target=i-gridRowsPerColumn;
        else if(code==juce::KeyPress::rightKey&&i+gridRowsPerColumn<count) target=i+gridRowsPerColumn;
        else if(code==juce::KeyPress::pageUpKey) target=juce::jmax(first,i-5);
        else if(code==juce::KeyPress::pageDownKey) target=juce::jmin(last,i+5);
        else if(code==juce::KeyPress::homeKey) target=0;
        else if(code==juce::KeyPress::endKey) target=count-1;
        if(target!=i) selectGridRow(target,true);
    }
    bool handleKey(const juce::KeyPress& k,juce::Component* origin) {
        origin=keyboardControl(origin);
        const auto code=k.getKeyCode(); const auto mods=k.getModifiers();
        const auto ch=juce::CharacterFunctions::toLowerCase(code>='A'&&code<='Z'?static_cast<juce::juce_wchar>(code):k.getTextCharacter()!=0?k.getTextCharacter():static_cast<juce::juce_wchar>(code));
        const bool alt=mods.isAltDown(), ctrl=mods.isCtrlDown()||mods.isCommandDown();
        const bool vertical=code==juce::KeyPress::upKey||code==juce::KeyPress::downKey;
        const bool horizontal=code==juce::KeyPress::leftKey||code==juce::KeyPress::rightKey;
        const bool page=code==juce::KeyPress::pageUpKey||code==juce::KeyPress::pageDownKey;
        const bool boundary=code==juce::KeyPress::homeKey||code==juce::KeyPress::endKey;
        const int direction=code==juce::KeyPress::upKey||code==juce::KeyPress::pageUpKey||code==juce::KeyPress::homeKey||code==juce::KeyPress::rightKey?1:-1;
        // Leave the host and the native combo popup in control of their own keys.
        if(chooser||grid.isPopupActive()||bank.isPopupActive()||program.isPopupActive()||channel.isPopupActive()) return false;
        if(alt&&!ctrl&&!mods.isShiftDown()) {
            search.reset();
            if(gridOpen&&(origin==&grid||origin==&value)) {
                if(horizontal) {changeStep(*origin,direction);return true;}
                if(vertical||page||boundary) {editSelected(*origin,direction,page,boundary);return true;}
            }
            if(!gridOpen&&origin==&level) {
                if(horizontal) {changeStep(level,direction);return true;}
                if(vertical||page||boundary) {changeMainLevel(direction,page,boundary);return true;}
            }
            if(ch=='l') {openParameters();return true;}
            if(ch=='v'&&gridOpen) {focus(value);return true;}
            if(ch=='c'&&gridOpen) {closeParameters();return true;}
            juce::Component* target=nullptr;
            if(ch=='o') target=&open; else if(ch=='m') target=&channel; else if(ch=='b') target=&bank; else if(ch=='p') target=&program; else if(ch=='g') target=&level; else if(ch=='a') target=&stop; else if(ch=='s') target=&readStatus;
            if(target) {if(target->isEnabled()) {if(gridOpen) showGrid(false,target); focus(*target);} return true;}
        }
        if(gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::escapeKey) {closeParameters();return true;}
        if(alt||ctrl) {
            if(!alt&&ctrl&&gridOpen&&origin==&grid&&boundary) {search.reset();navigateGrid(code);return true;}
            return false;
        }
        if(gridOpen&&(origin==&grid||origin==&value)) {
            const bool edit=origin==&value;
            if(code==juce::KeyPress::returnKey) {search.reset();focus(edit?static_cast<juce::Component&>(grid):static_cast<juce::Component&>(value));return true;}
            if(edit) {
                search.reset();
                if(horizontal) {changeStep(value,direction);return true;}
                if(vertical||page||boundary) {editSelected(value,direction,page,boundary);return true;}
            } else {
                if(vertical||horizontal||page||boundary) {search.reset();navigateGrid(code);return true;}
                if(const auto character=ljunoParameterSearchCharacter(k);character!=0) {
                    const int current=juce::jmax(0,grid.getSelectedItemIndex());
                    const int found=search.next(static_cast<wchar_t>(character),current,static_cast<int>(rows.size()),mods.isShiftDown()?-1:1,juce::Time::getMillisecondCounter(),[this](int index){return std::wstring(rows[static_cast<size_t>(index)].name.toWideCharPointer());});
                    if(found>=0&&found!=current) selectGridRow(found,true);
                    return true; // never fall back to ComboBox's own type-ahead
                }
                search.reset();
            }
            if(code==juce::KeyPress::backspaceKey) {
                if(auto* r=selectedRow()) {
                    const auto copy=*r; const int f=fxIndex(copy.id); double reset=0; bool supported=true;
                    if(f>=0) reset=fxSpecs[f].initial;
                    else if(copy.id>=128) reset=0;
                    else if(copy.id==rowGain) reset=-6;
                    else if(copy.id==rowInterpolation) reset=1;
                    else if(copy.id==rowEffects||copy.id==rowSfzQuality) reset=0;
                    else if(copy.id==rowMidiFX) reset=1;
                    else supported=false;
                    if(supported) {writeRow(copy,reset);refreshGridValues();speak(*origin,formatValue(copy,readRow(copy)));}
                }
                return true;
            }
        }
        if(origin==&level) {
            if(horizontal) {changeStep(level,direction);return true;}
            if(vertical||page||boundary) {changeMainLevel(direction,page,boundary);return true;}
        }
        if(dynamic_cast<juce::Button*>(origin)&&(horizontal||vertical||page||boundary)) return true;
        return false;
    }
    void browse() {
        if(chooser) return;
        auto start=cached.path.isNotEmpty()?juce::File(cached.path).getParentDirectory():juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser=std::make_unique<juce::FileChooser>("Open SF2 or SFZ",start,"*.sf2;*.sfz",true);
        juce::Component::SafePointer<Editor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& fc) {
            if(auto* self=safe.getComponent()) { auto file=fc.getResult(); if(file!=juce::File{}) self->proc.loadFile(file); juce::MessageManager::callAsync([safe]{if(auto* e=safe.getComponent()) {e->chooser.reset();e->initialFocusPending=true;e->scheduleInitialFocus();}}); }
        });
    }
    void rebuildPrograms(int b,int p) {
        program.clear(juce::dontSendNotification);
        for(const auto& item:cached.presets) if(item.bank==b) program.addItem(juce::String(item.program)+" - "+item.name,item.program+1);
        if(p>=0) program.setSelectedId(p+1,juce::dontSendNotification);
    }
    void apply() {
        if(bank.getSelectedId()>0 && program.getSelectedId()>0) {
            int b=bank.getSelectedId()-1,p=program.getSelectedId()-1;
            proc.select(selectedChannel(),b,p); shownCode=pack(b,p);
        }
    }
    void refresh(bool force) {
        const bool changed=proc.getRevision()!=revision;
        if(changed) {cached=proc.getView();revision=cached.revision;status.setText(cached.status,juce::dontSendNotification);status.setDescription(cached.status);}
        const int code=proc.current(selectedChannel());
        if(force||changed||code!=shownCode) {
            if(!force&&!changed&&(bank.isPopupActive()||program.isPopupActive())) return;
            updating=true;
            if(changed||force) {
                bank.clear(juce::dontSendNotification); int last=-1;
                for(const auto& p:cached.presets) if(p.bank!=last) {bank.addItem("Bank "+juce::String(p.bank),p.bank+1);last=p.bank;}
            }
            bank.setSelectedId((code>>7)+1,juce::dontSendNotification); rebuildPrograms(code>>7,code&127); shownCode=code;
            updating=false;
        }
        const bool ready=cached.ready&&!cached.loading&&!cached.presets.empty(); bank.setEnabled(ready);program.setEnabled(ready);channel.setEnabled(!cached.isSfz&&!cached.loading);
        if(!dragging) { const auto gain=proc.gainParameter().get(); if(std::abs(level.getValue()-gain)>0.000001) level.setValue(gain,juce::dontSendNotification); const auto text=juce::String(gain,1)+" dB"; if(levelReadout.getText()!=text) levelReadout.setText(text,juce::dontSendNotification); }
        if(gridOpen) {if(changed) rebuildGrid(); else refreshGridValues();}
    }
    void timerCallback() override {refresh(false); if(initialFocusPending&&isShowing()&&getPeer()!=nullptr&&getPeer()->isFocused()) scheduleInitialFocus();}
};
juce::AudioProcessorEditor* Processor::createEditor(){return new Editor(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Processor();}
