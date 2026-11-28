#include <JuceHeader.h>
#include <fluidsynth.h>
#include <sfizz.h>
#include "OutputStage.h"
#include "ChannelCompressor.h"
#include "SF2ChannelTrim.h"
#include "MidiEffects.h"
#include "ParameterSearchKey.h"
#include "PresetBrowserMemory.h"
#include "GridNavigation.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <map>
#include <iterator>
#include "VirtualProgramMap.h"
#include "NoteAssignments.h"
#include "NoteRouteGroups.h"
extern "C" int lsfx_fluid_noteon(fluid_synth_t*,int,int,int,int,int,float,float,float);
extern "C" int lsfx_fluid_channel_dry(fluid_synth_t*,int,int);
#include "SoundFontExport.h"
#include "SfzSoundFontExport.h"
#include "InstrumentProjectIO.h"
#include "InstrumentPreview.h"
#if JUCE_WINDOWS
 #include <Windows.h>
 #include <UIAutomation.h>
 #include <oleauto.h>
#endif

namespace {
lsfx::sfzexport::Audio decodeSfzExportSample(juce::AudioFormatManager& formats,const lsfx::sf2export::Path& path,const std::atomic<bool>* cancel) {
    auto sampleFile=juce::File(juce::String::fromUTF8(path.u8string().c_str()));
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sampleFile));
    if(!reader) throw std::runtime_error("Cannot decode SFZ sample: "+path.u8string());
    lsfx::sfzexport::Audio a;
    if(reader->numChannels<1||reader->numChannels>2||reader->lengthInSamples<=0||reader->lengthInSamples>100000000)
        throw std::runtime_error("Unsupported SFZ sample length or channel count: "+path.u8string());
    a.channels=int(reader->numChannels); a.rate=int(reader->sampleRate);
    const auto& meta=reader->metadataValues;
    a.root=meta.getValue("MidiUnityNote","60").getIntValue();
    if(meta.getValue("NumSampleLoops","0").getIntValue()>0) {
        a.loopStart=meta.getValue("Loop0Start","-1").getLargeIntValue();
        a.loopEnd=meta.getValue("Loop0End","-1").getLargeIntValue()+1;
    }
    a.pcm.resize(size_t(reader->lengthInSamples)*size_t(a.channels));
    juce::AudioBuffer<float> block(a.channels,32768);
    for(juce::int64 pos=0;pos<reader->lengthInSamples;) {
        lsfx::sf2export::checkCancel(cancel);
        int n=int(std::min<juce::int64>(32768,reader->lengthInSamples-pos));
        if(!reader->read(&block,0,n,pos,true,a.channels==2)) throw std::runtime_error("Cannot read SFZ audio sample");
        for(int i=0;i<n;++i)for(int ch=0;ch<a.channels;++ch)a.pcm[size_t(pos+i)*size_t(a.channels)+size_t(ch)]=block.getSample(ch,i);
        pos+=n;
    }
    return a;
}
constexpr int channels = 16;
constexpr auto lsfxVersion="0.99.3 TEST37";
constexpr auto lsfxReleaseDate="28 November 2026";
constexpr auto lsfxProjectUrl="https://github.com/Lo-lo78/LSFX";
constexpr auto lsfxContactEmail="vmanolo301@gmail.com";
constexpr auto lsfxLicense="GNU Affero General Public License v3 or later (AGPL-3.0-or-later)";
const juce::String settingsName="LSFX";
juce::File presetRoot() { return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("LSFX"); }
juce::String setting(const juce::String& key) { juce::PropertiesFile f(preset_browser_memory::options(settingsName)); return f.getValue(key); }
void rememberSetting(const juce::String& key,const juce::String& value) { juce::PropertiesFile f(preset_browser_memory::options(settingsName)); f.setValue(key,value); f.saveIfNeeded(); }
bool insideLibrary(const juce::File& f) { const auto root=presetRoot(); return f==root||f.isAChildOf(root); }
juce::File savedDirectory() { const auto path=setting("saveDirectory"); if(juce::File::isAbsolutePath(path)) { juce::File f(path); if(insideLibrary(f)&&f.isDirectory()) return f; } return presetRoot(); }

constexpr int stereoBuses=lsfx::stereoOutputCount, renderChunk=1024;
constexpr int sf2Groups=lsfx::NoteRouteGroups::count;
// TEST26.1: multigroup is the default validation build. The stereo-only
// recovery build remains selectable at configure time. Windows host stability
// is not established by the standalone tests.
#ifndef LSFX_EXPERIMENTAL_SF2_MULTIGROUP
 #define LSFX_EXPERIMENTAL_SF2_MULTIGROUP 0
#endif
constexpr bool sf2MultiOutput=LSFX_EXPERIMENTAL_SF2_MULTIGROUP!=0;
juce::String compressorId(int ch,int param) {return "channelCompressor_"+juce::String(ch+1)+"_"+lsfx::compSpecs[param].key;}
juce::String routeName(int bus) { return bus==0?juce::String("Main 1/2"):juce::String(2*bus+1)+"/"+juce::String(2*bus+2); }
juce::String routeId(int ch) { return "channelRoute"+juce::String(ch+1); }
juce::StringArray routeChoices() { juce::StringArray a; for(int i=0;i<stereoBuses;++i) a.add(routeName(i)); return a; }
constexpr float channelGainMin=-60.0f, channelGainMax=6.0f;
constexpr float sfzGainMin=-60.0f, sfzGainMax=24.0f;
using InstrumentGains=std::map<int,float>;
juce::String channelGainId(int ch) { return "channelGain"+juce::String(ch+1); }
// Stable indices for global host parameters and XML attributes.
enum FXParam { rvType, rvLevel, rvTime, rvDamp, rvWidth, chType, chLevel, chRate, chDepth, chFeedback, delayMode, delayTime, delayFeedback, delayMix, inputTrim, drumTrim, sf2Dynamics, sf2Ambience, fxCount };
struct FXSpec { const char* key; const char* label; int lo,hi,initial; const char* unit; };
constexpr FXSpec fxSpecs[] = {
    {"reverbType","Reverb type",0,7,2,""}, {"reverbLevel","Reverb level",0,100,70," %"}, {"reverbTime","Reverb time scale",0,100,50," %"}, {"reverbDamp","Reverb damping",0,100,30," %"}, {"reverbWidth","Reverb width",0,100,80," %"},
    {"chorusType","Chorus type",0,7,1,""}, {"chorusLevel","Chorus level",0,100,55," %"}, {"chorusRate","Chorus rate",10,500,36," x 0.01 Hz"}, {"chorusDepth","Chorus depth scale",0,100,45," %"}, {"chorusFeedback","Chorus feedback",0,85,25," %"},
    {"delayMode","Delay mode",0,1,0,""}, {"delayTime","Delay time",1,1000,250," ms"}, {"delayFeedback","Delay feedback",0,85,30," %"}, {"delayMix","Delay level",0,100,0," %"}, {"inputTrim","Output stage input trim",0,24,0," dB"},
    {"drumTrim","Legacy channel 10 trim (inactive)",-12,12,0," dB"}, {"sf2Dynamics","SF2 master dynamics",0,2,2,""},
    {"sf2Ambience","SF2 initial ambience",0,1,0,""}
};
static_assert(sizeof(fxSpecs)/sizeof(fxSpecs[0])==fxCount);
using FXValues=std::array<int,fxCount>;
constexpr int globalRowBase=-100;
int fxRow(int i) { return globalRowBase-i; }
int fxIndex(int row) { const int i=globalRowBase-row; return i>=0&&i<fxCount?i:-1; }
const char* const reverbNames[]={"Room 1","Room 2","Room 3","Hall 1","Hall 2","Plate","Delay","Panning Delay"};
const char* const chorusNames[]={"Chorus 1","Chorus 2","Chorus 3","Chorus 4","Feedback Chorus","Flanger","Short Delay","Short Delay with Feedback"};
float roomSize(const FXValues& f) { constexpr float rooms[]={.22f,.36f,.50f,.62f,.76f,.57f}; return std::clamp(rooms[(std::min)(f[rvType],5)]*(.5f+.01f*f[rvTime]),.02f,.88f); }
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
struct Preset { int bank, program; juce::String name; juce::String path; int sourceBank=0, sourceProgram=0, font=-1, sfzIndex=-1; };
struct MapEdit { int bank=0, program=-1, sourceBank=0, sourceProgram=0; juce::String path; bool clear=false; };
using MapEdits=std::vector<MapEdit>;
static juce::String mapDisplayPath(const juce::String& base,const MapEdits& edits) {
    if(base.isNotEmpty())return base;
    for(const auto& e:edits)if(!e.clear&&e.path.isNotEmpty())return e.path;
    return {};
}
const Preset* findPreset(const std::vector<Preset>& list,int code) {
    const auto it=std::lower_bound(list.begin(),list.end(),code,[](const Preset& a,int key){return pack(a.bank,a.program)<key;});
    return it!=list.end()&&pack(it->bank,it->program)==code?&*it:nullptr;
}
std::vector<Preset> catalogSF2(fluid_synth_t* synth,int font,const juce::String& path) {
    std::vector<Preset> result;
    if(auto* sf=fluid_synth_get_sfont_by_id(synth,font)) {
        fluid_sfont_iteration_start(sf);
        while(auto* p=fluid_sfont_iteration_next(sf)) {
            const int bank=fluid_preset_get_banknum(p), program=fluid_preset_get_num(p);
            if(lsfx::validAddress(bank,program)) result.push_back({bank,program,juce::String::fromUTF8(fluid_preset_get_name(p)),path,bank,program,font});
        }
    }
    std::sort(result.begin(),result.end(),[](const Preset& a,const Preset& b){return pack(a.bank,a.program)<pack(b.bank,b.program);});
    return result;
}
juce::String sourceName(const juce::String& path) {
    const juce::File f(path);
    return (f.hasFileExtension("sfz")?juce::String("SFZ "):juce::String("SF2 "))+f.getFileNameWithoutExtension();
}
struct CatalogResult { std::vector<Preset> presets; juce::String error; };
CatalogResult inspectSF2(const juce::File& file) {
    CatalogResult result;
    if(!file.existsAsFile()) {result.error="SoundFont not found: "+file.getFullPathName();return result;}
    std::unique_ptr<fluid_settings_t,decltype(&delete_fluid_settings)> settings(new_fluid_settings(),delete_fluid_settings);
    if(!settings) {result.error="Unable to create FluidSynth settings";return result;}
    fluid_settings_setint(settings.get(),"synth.midi-channels",16);
    fluid_settings_setint(settings.get(),"synth.threadsafe-api",0);
    fluid_settings_setint(settings.get(),"synth.cpu-cores",1);
    fluid_settings_setint(settings.get(),"synth.audio-channels",1);
    fluid_settings_setint(settings.get(),"synth.audio-groups",1);
    fluid_settings_setint(settings.get(),"synth.effects-groups",1);
    fluid_settings_setint(settings.get(),"synth.reverb.active",0);
    fluid_settings_setint(settings.get(),"synth.chorus.active",0);
    std::unique_ptr<fluid_synth_t,decltype(&delete_fluid_synth)> synth(new_fluid_synth(settings.get()),delete_fluid_synth);
    if(!synth) {result.error="Unable to create FluidSynth";return result;}
    const int font=fluid_synth_sfload(synth.get(),file.getFullPathName().toRawUTF8(),0);
    if(font<0) {result.error="Unable to load SoundFont: "+file.getFullPathName();return result;}
    result.presets=catalogSF2(synth.get(),font,file.getFullPathName());
    if(result.presets.empty()) result.error="No presets found: "+file.getFileName();
    return result;
}
struct SfzSlot {
    sfizz_synth_t* synth=nullptr;
    int owner=-1, defaultQuality=0, appliedQuality=-1;
    bool used=false;
    juce::SmoothedValue<float> instrumentGain;
    bool gainPrimed=false;
    ~SfzSlot() { if(synth) sfizz_free(synth); }
    void stop() {
        sfizz_send_cc(synth,0,64,0); sfizz_send_cc(synth,0,123,0);
        for(int key=0;key<128;++key) sfizz_send_note_off(synth,0,key,0);
        sfizz_send_cc(synth,0,120,0);
    }
};
struct SfzSource {
    juce::String path;
    std::vector<CCLabel> labels;
    std::array<int,120> initialCC{};
    std::array<std::unique_ptr<SfzSlot>,channels> pool;
};
std::unique_ptr<SfzSlot> createSfz(const juce::String& path,double rate,std::vector<CCLabel>* labels=nullptr,std::array<int,120>* initialCC=nullptr) {
    auto slot=std::make_unique<SfzSlot>();
    slot->synth=sfizz_create_synth();
    if(!slot->synth) return {};
    auto* z=slot->synth;
    sfizz_set_sample_rate(z,static_cast<float>(rate));
    sfizz_set_samples_per_block(z,renderChunk);
    sfizz_set_num_voices(z,128);
    if(!sfizz_load_file(z,path.toRawUTF8())) return {};
    slot->defaultQuality=sfizz_get_sample_quality(z,SFIZZ_PROCESS_LIVE);
    if(labels) {
        if(initialCC) initialCC->fill(unset);
        const auto n=sfizz_get_num_cc_labels(z);
        for(unsigned i=0;i<n;++i) {
            const int cc=sfizz_get_cc_label_number(z,static_cast<int>(i));
            if(cc<=0||cc==32||cc>=120) continue;
            const char* text=sfizz_get_cc_label_text(z,static_cast<int>(i));
            if(text&&*text) labels->push_back({cc,juce::String::fromUTF8(text)});
        }
        std::sort(labels->begin(),labels->end(),[](const CCLabel& a,const CCLabel& b){return a.cc<b.cc;});
        labels->erase(std::unique(labels->begin(),labels->end(),[](const CCLabel& a,const CCLabel& b){return a.cc==b.cc;}),labels->end());
        int reply=unset;
        if(auto* client=sfizz_create_client(&reply)) {
            sfizz_set_receive_callback(client,[](void* data,int,const char*,const char* sig,const sfizz_arg_t* args){
                if(sig&&sig[0]=='f'&&args&&std::isfinite(args[0].f))
                    *static_cast<int*>(data)=juce::jlimit(0,127,juce::roundToInt(args[0].f*127.0f));
            });
            if(initialCC) for(int cc=1;cc<120;++cc) if(cc!=32) {
                reply=unset;
                const auto address="/cc"+juce::String(cc)+"/value";
                sfizz_send_message(z,client,0,address.toRawUTF8(),"",nullptr);
                (*initialCC)[cc]=reply;
            }
            for(auto& cc:*labels) {
                if(initialCC) cc.initial=(*initialCC)[cc.cc];
                else {
                    reply=unset;
                    const auto address="/cc"+juce::String(cc.cc)+"/value";
                    sfizz_send_message(z,client,0,address.toRawUTF8(),"",nullptr);
                    cc.initial=reply;
                }
            }
            sfizz_delete_client(client);
        }
    }
    return slot;
}
struct Engine {
    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    std::vector<SfzSource> sfzSources;
    std::array<SfzSlot*,channels> sfzActive{};
    std::array<int,channels> sfzAddress{};
    Controls sfzCC=emptyControls();
    std::array<int,channels> resetPrograms{}; // initial addresses, independent of current MIDI state
    uint64_t serial=0;
    bool hasSF2=false;
    bool hasSFZ() const { return !sfzSources.empty(); }
    lsfx::StereoDelay sfzModulation;
    std::array<float,renderChunk> sfzL{},sfzR{},sfzMainL{},sfzMainR{};
    void releaseSfz(int ch) {
        if(auto* slot=sfzActive[ch]) { slot->stop(); slot->owner=-1; sfzActive[ch]=nullptr; }
        sfzAddress[ch]=-1;
    }
    bool activateSfz(int ch,int code,const Preset* p) {
        if(sfzActive[ch]&&sfzAddress[ch]==code) return true;
        releaseSfz(ch);
        if(!p||p->sfzIndex<0) return true;
        auto& source=sfzSources[static_cast<size_t>(p->sfzIndex)];
        for(auto& candidate:source.pool) if(candidate&&candidate->owner<0) {
            auto& slot=*candidate; slot.owner=ch; slot.gainPrimed=false;
            sfzActive[ch]=&slot; sfzAddress[ch]=code;
            if(slot.used) {
                // Restore instrument defaults when reusing an instance on another channel.
                sfizz_send_cc(slot.synth,0,121,0);
                sfizz_send_hd_pitch_wheel(slot.synth,0,0.0f);
                sfizz_send_channel_aftertouch(slot.synth,0,0);
                for(int cc=1;cc<120;++cc) if(cc!=32&&source.initialCC[cc]!=unset)
                    sfizz_send_cc(slot.synth,0,cc,source.initialCC[cc]);
            }
            slot.used=true;
            for(int cc=1;cc<120;++cc) if(cc!=32&&sfzCC[controlIndex(ch,cc)]!=unset) sfizz_send_cc(slot.synth,0,cc,sfzCC[controlIndex(ch,cc)]);
            return true;
        }
        return false;
    }
    int quality = -1, effects = -1;
    int quietSamples = 0;
    int midiDisabledFx=0;
    int lastMidiFxMode=-1;
    float midiMaster=1.0f;
    FXValues lastFx{};
    lsfx::OutputStage output;
    // Distinct audio/FX planes; never alias pointers passed to FluidSynth.
    std::array<std::array<float,renderChunk>,2*sf2Groups> dry{};
    lsfx::NoteRouteGroups noteGroups;
    std::array<std::array<float,renderChunk>,4> wet{};
    std::array<int,channels> routes{};
    // Preserve native voice trim and its MIDI semantics.
    std::array<lsfx::SF2ChannelTrim,channels> sf2Trims;
    std::array<lsfx::ChannelCompressor,channels> compressors;
    std::array<juce::SmoothedValue<float,juce::ValueSmoothingTypes::Linear>,channels> channelGains;
    void prepareMixer() {
        for(auto& g:channelGains) { g.reset(rate,0.005); g.setCurrentAndTargetValue(1.0f); }
        for(auto& c:compressors) c.prepare(rate);
    }
    // Ambience defaults apply only to channels without an explicit CC91.
    std::array<bool,channels> reverbSendExplicit{};
    int ambienceApplied=0;
    lsfx::StereoDelay reverbDelay, modulation, delay;
    juce::Reverb sfzReverb;
    void prepareFX() { lastFx.fill(-1); output.prepare(rate); reverbDelay.prepare(rate); modulation.prepare(rate); delay.prepare(rate); sfzReverb.setSampleRate(rate); sfzModulation.prepare(rate); sfzAddress.fill(-1); }
    void resetFX() { for(auto& c:compressors) c.reset(); output.reset(); reverbDelay.reset(); modulation.reset(); delay.reset(); sfzReverb.reset(); sfzModulation.reset(); quietSamples=0; }

    int font = -1;
    std::vector<Preset> map;
    double rate = 44100;
    ~Engine() { if (synth) delete_fluid_synth(synth); if (settings) delete_fluid_settings(settings); }
};
using Programs = std::array<int, channels>;
Programs defaults() { Programs p{}; p[9] = pack(128, 0); return p; }
// Keep the same initial selection policy as opening an ordinary source.
Programs initialPrograms(bool standaloneSfz,int address=0) {
    auto p=defaults();
    if(standaloneSfz) {
        p[0]=address;
        for(int ch=1;ch<channels;++ch) if(p[ch]==address) p[ch]=pack(address>>7,((address&127)+1)&127);
    }
    return p;
}
struct Request { juce::String path; double rate = 44100; Programs programs = defaults(); uint64_t serial = 0; Controls controls=emptyControls(); MapEdits edits; int sfzCode=0; std::optional<InstrumentGains> restoredGains; int resetGainBank=-1,resetGainProgram=-1; std::optional<lsfx::NoteAssignments> restoredNotes; };
struct View { juce::String path, status = "No SoundFont loaded"; std::vector<Preset> presets; uint64_t revision = 0; bool loading = false; bool ready=false; std::map<juce::String,std::vector<CCLabel>> labels; };
}

class Processor final : public juce::AudioProcessor {
public:
    static BusesProperties makeBuses() {
        BusesProperties p;
        p=p.withOutput("Main 1/2",juce::AudioChannelSet::stereo(),true);
        for(int i=1;i<stereoBuses;++i) p=p.withOutput("Output "+routeName(i),juce::AudioChannelSet::stereo(),true);
        return p;
    }
    Processor() : AudioProcessor(makeBuses()) {
        auto p = defaults();
        for (int i=0; i<channels; ++i) { selected[i].store(p[i]); commands[i].store(-1); sfzNeeded[i].store(-1); sfzRetry[i].store(-1); }
        addParameter(gain = new juce::AudioParameterFloat({"gain",1}, "Master output gain", juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), 0.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
        addParameter(quality = new juce::AudioParameterChoice({"quality",1}, "SF2 interpolation", {"Linear", "Fourth order", "Seventh order"}, 1));
        addParameter(effects = new juce::AudioParameterChoice({"effects",1}, "Effects", {"Off", "Reverb", "Chorus", "Both"}, 0));
        addParameter(sfzQuality = new juce::AudioParameterChoice({"sfzQuality",1}, "SFZ live sample quality", {"Engine default","0","1","2","3","4","5","6","7","8","9","10"},0));
        for(int i=0;i<sf2Ambience;++i) { const auto& f=fxSpecs[i];
            if(i==rvType||i==chType||i==delayMode||i==sf2Dynamics) {
                juce::StringArray choices;
                if(i==sf2Dynamics) { choices.add("LJuno Glue"); choices.add("Linear (soft ceiling)"); choices.add("Reference (unprocessed)"); }
                else if(i==delayMode) { choices.add("Stereo"); choices.add("Ping-pong"); }
                else for(int k=0;k<8;++k) choices.add(i==rvType?reverbNames[k]:chorusNames[k]);
                addParameter(fxParams[i]=new juce::AudioParameterChoice({f.key,1},f.label,choices,f.initial));
            } else addParameter(fxParams[i]=new juce::AudioParameterInt({f.key,1},f.label,f.lo,f.hi,f.initial));
        }
        addParameter(midiFX = new juce::AudioParameterChoice({"midiFX",1}, "MIDI effects reception", {"Manual", "Follow MIDI"},1));
        // Append after MIDI FX so existing host parameter order is unchanged.
        addParameter(fxParams[sf2Ambience]=new juce::AudioParameterChoice({"sf2Ambience",1}, "SF2 initial ambience", {"Native MIDI", "Scoring ambience (CC91 40)"},0));
        // Append, preserving all existing parameter identifiers and ordering.
        for(int ch=0;ch<channels;++ch) addParameter(channelGain[ch]=new juce::AudioParameterFloat({channelGainId(ch),1}, "Output gain CH "+juce::String(ch+1), juce::NormalisableRange<float>(channelGainMin,channelGainMax,0.1f),0.0f,juce::AudioParameterFloatAttributes().withLabel("dB")));
        for(int ch=0;ch<channels;++ch) addParameter(channelRoute[ch]=new juce::AudioParameterChoice({routeId(ch),1}, "Output routing CH "+juce::String(ch+1),routeChoices(),0));
        // Append new automation parameters: preserve all existing IDs/order.
        for(int ch=0;ch<channels;++ch) for(int i=0;i<lsfx::compCount;++i) {
            const auto& spec=lsfx::compSpecs[i];
            const auto name=juce::String(spec.name)+" CH "+juce::String(ch+1);
            if(i==lsfx::compRatio) {
                juce::StringArray choices; for(auto* item:lsfx::compRatioNames) choices.add(item);
                addParameter(compParams[ch][i]=new juce::AudioParameterChoice({compressorId(ch,i),1},name,choices,int(spec.initial)));
            } else addParameter(compParams[ch][i]=new juce::AudioParameterFloat({compressorId(ch,i),1},name,juce::NormalisableRange<float>(spec.lo,spec.hi,spec.step),spec.initial,juce::AudioParameterFloatAttributes().withLabel(juce::String(spec.unit).trim())));
        }
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
    bool isBusesLayoutSupported(const BusesLayout& l) const override {
        if(l.getMainOutputChannelSet()!=juce::AudioChannelSet::stereo()) return false;
        for(const auto& set:l.outputBuses) if(!set.isDisabled()&&set!=juce::AudioChannelSet::stereo()) return false;
        return true;
    }
    void prepareToPlay(double rate, int) override {
        const auto old = hostRate.exchange(rate);
        instrumentPreview.setRate(rate);
        if (std::abs(old-rate)>0.01) {
            juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0;
            InstrumentGains savedGains; lsfx::NoteAssignments savedNotes; stateForSave(path,p,&c,&edits,&sfzCode,&savedGains,&savedNotes);
            if (path.isNotEmpty()||!edits.empty()) load(path,p,c,std::move(edits),sfzCode,false,std::move(savedGains),-1,-1,std::move(savedNotes));
        }
    }
    void releaseResources() override { panic.store(true); instrumentPreview.panic(); }
    void reset() override { panic.store(true); instrumentPreview.panic(); }
    uint64_t getRevision() { std::lock_guard<std::mutex> lock(viewMutex); return view.revision; }
    View getView() { std::lock_guard<std::mutex> lock(viewMutex); return view; }
    int current(int ch) const { const auto cmd=commands[ch].load(); return cmd>=0 ? cmd : selected[ch].load(); }
    void select(int ch,int bank,int prog) { if (ch>=0 && ch<channels && bank>=0 && bank<=16383 && prog>=0 && prog<128) commands[ch].store(pack(bank,prog)); }
    void allOff() { panic.store(true); }
    // This is a MIDI reset, not an instrument reload or a mixer reset.
    bool resetMidi() {
        std::lock_guard<std::mutex> lock(requestMutex);
        if(pending||loadingSerial!=0||failedSerial==latest.serial||(latest.path.isEmpty()&&latest.edits.empty())) return false;
        overrides=emptyControls();
        for(auto& c:controlCommands) c.store(unset);
        controlDirty.store(false);
        for(auto& c:commands) c.store(-1);
        midiResetRequested.store(true);
        return true;
    }
    // Full instance initialization, not just MIDI panic. Keep user-wide
    // browser settings and on-disk presets intact. Worker retires the engine.
    void initializeInstance() {
        clearing.store(true);
        panic.store(true);
        setCurrentPresetFile(juce::File{});
        setUiChannel(0);
        load({},defaults(),emptyControls(),{},0,false,InstrumentGains{},-1,-1,lsfx::NoteAssignments{});
        for(auto* parameter:getParameters()) {
            if(parameter->getValue()!=parameter->getDefaultValue()) {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost(parameter->getDefaultValue());
                parameter->endChangeGesture();
            }
        }
    }
    // Editor-only state belongs to this processor instance, not to the temporary editor.
    struct EditorMemory { int focus=1,gridReturn=2,gainChannel=0,routeChannel=0,sf2Row=-1,sfzRow=-1,emptyRow=-1,step=0; };
    EditorMemory editorMemory;
    lsfx::instrument::Draft instrumentDraft; // message thread only; no DSP access
    lsfx::instrument::InstrumentPreview instrumentPreview;
    void previewInstrument(const lsfx::instrument::Project& p,int raw=-1) {panic.store(true);instrumentPreview.update(p,hostRate.load(),raw);}
    void stopInstrumentPreview(){panic.store(true);instrumentPreview.disable();}
    float instrumentGain(int code) {
        std::lock_guard<std::mutex> audio(engineMutex);
        const auto it=instrumentGains.find(code); return it==instrumentGains.end()?0.0f:it->second;
    }
    void setInstrumentGain(int code,float db) {
        std::lock_guard<std::mutex> audio(engineMutex);
        if(!engine||!std::isfinite(db)) return;
        const auto* preset=findPreset(engine->map,code);
        if(!preset||preset->sfzIndex<0) return;
        db=juce::jlimit(sfzGainMin,sfzGainMax,db);
        if(std::abs(db)<0.00001f) instrumentGains.erase(code); else instrumentGains[code]=db;
    }
    int uiChannel() const { return editorChannel.load(); }
    void setUiChannel(int ch) { editorChannel.store(juce::jlimit(0,channels-1,ch)); }
    void retryFile() { juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0; stateForSave(path,p,&c,&edits,&sfzCode); if(path.isNotEmpty()||!edits.empty()) load(path,p,c,std::move(edits),sfzCode); }
    juce::Result importSF2(const juce::File& file,int destBank,int destProgram,int sourceBank,int sourceProgram,int ch) {
        if(!lsfx::validAddress(destBank,destProgram<0?0:destProgram)||!lsfx::validAddress(sourceBank,sourceProgram)) return juce::Result::fail("Invalid bank or program");
        juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0;
        stateForSave(path,p,&c,&edits,&sfzCode);
        edits.erase(std::remove_if(edits.begin(),edits.end(),[&](const MapEdit& e){return e.bank==destBank&&(destProgram<0||e.program==destProgram);}),edits.end());
        edits.push_back({destBank,destProgram,sourceBank,sourceProgram,file.getFullPathName()});
        p[ch]=pack(destBank,destProgram<0?(p[ch]&127):destProgram);
        if(midiFX->getIndex()==1&&effects->getIndex()==0) effects->setValueNotifyingHost(effects->convertTo0to1(3.0f));
        load(path,p,c,std::move(edits),sfzCode,true,std::nullopt,destBank,destProgram); return juce::Result::ok();
    }
    juce::Result importSFZ(const juce::File& file,int code,int ch) {
        if(!lsfx::validAddress(code>>7,code&127)||ch<0||ch>=channels||!file.hasFileExtension("sfz")||!file.existsAsFile()) return juce::Result::fail("Invalid SFZ assignment");
        juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0;
        stateForSave(path,p,&c,&edits,&sfzCode);
        edits.erase(std::remove_if(edits.begin(),edits.end(),[&](const MapEdit& e){return e.bank==(code>>7)&&e.program==(code&127);}),edits.end());
        edits.push_back({code>>7,code&127,0,0,file.getFullPathName()});
        // Preserve the single-SFZ opening convention on a completely empty map.
        // Explicit assignments in a populated map keep their addresses unchanged.
        if(path.isEmpty()&&edits.size()==1) for(int i=0;i<channels;++i) if(i!=ch&&p[i]==code) p[i]=pack(code>>7,(code+1)&127);
        p[ch]=code; load(path,p,c,std::move(edits),sfzCode,true,std::nullopt,code>>7,code&127); return juce::Result::ok();
    }
    juce::Result clearLocation(int bank,int program) {
        if(program<-1||!lsfx::validAddress(bank,program<0?0:program)) return juce::Result::fail("Invalid destination");
        const auto v=getView();
        if(v.loading) return juce::Result::fail("Wait for the current load to finish");
        const auto matches=[&](const Preset& item){return item.bank==bank&&(program<0||item.program==program);};
        const auto count=std::count_if(v.presets.begin(),v.presets.end(),matches);
        if(count==0) return juce::Result::ok();
        juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0;
        stateForSave(path,p,&c,&edits,&sfzCode);
        // If this was the last mapped material, release the entire engine and all sources.
        if(static_cast<size_t>(count)==v.presets.size()) {
            load({},p,c,{},0,true,InstrumentGains{},-1,-1,lsfx::NoteAssignments{});
        } else {
            edits.erase(std::remove_if(edits.begin(),edits.end(),[&](const MapEdit& e){return e.bank==bank&&(program<0||e.program==program);}),edits.end());
            // Tombstone masks the original base map or a previously imported bank.
            edits.push_back({bank,program,0,0,{},true});
            load(path,p,c,std::move(edits),sfzCode,true,std::nullopt,bank,program);
        }
        return juce::Result::ok();
    }
    std::map<int,lsfx::NoteAssignment> notesForProgram(int code) {
        std::lock_guard<std::mutex> lock(noteMutex);
        auto it=noteAssignments.find(code);return it==noteAssignments.end()?std::map<int,lsfx::NoteAssignment>{}:it->second;
    }
    void setNote(int code,int note,lsfx::NoteAssignment value) {
        std::lock_guard<std::mutex> audio(engineMutex);std::lock_guard<std::mutex> lock(noteMutex);lsfx::setNoteAssignment(noteAssignments,code,note,value);
    }
    void clearNotes(int code) {std::lock_guard<std::mutex> audio(engineMutex);std::lock_guard<std::mutex> lock(noteMutex);noteAssignments.erase(code);}
    void setNoteLearnChannel(int ch) noexcept {noteLearnChannel.store(ch);}
    uint64_t lastLearnedNote() const noexcept {return learnedNote.load();}
    juce::File currentPresetFile() { std::lock_guard<std::mutex> lock(presetMutex); return currentPreset; }
    void setCurrentPresetFile(const juce::File& f) { std::lock_guard<std::mutex> lock(presetMutex); currentPreset=f; }
    juce::Result savePresetFile(const juce::File& f) {
        if(!f.isAChildOf(presetRoot())||!f.hasFileExtension("lsfx")) return juce::Result::fail("Invalid preset path");
        if(auto result=f.getParentDirectory().createDirectory(); result.failed()) return result;
        juce::MemoryBlock state; getStateInformation(state);
        auto xml=getXmlFromBinary(state.getData(),static_cast<int>(state.getSize()));
        if(!xml) return juce::Result::fail("Cannot create preset state");
        xml->removeAttribute("presetFile");
        if(!f.replaceWithText(xml->toString())) return juce::Result::fail("Cannot write preset: "+f.getFullPathName());
        setCurrentPresetFile(f); return juce::Result::ok();
    }
    juce::Result readPresetFile(const juce::File& f,juce::MemoryBlock& state,juce::String& missing) {
        missing.clear();
        if(!f.isAChildOf(presetRoot())||!f.hasFileExtension("lsfx")||!f.existsAsFile()) return juce::Result::fail("Preset not found");
        juce::XmlDocument doc(f); auto xml=doc.getDocumentElement();
        if(!xml||!xml->hasTagName("LSoundFontState")) return juce::Result::fail("Invalid LSFX preset");
        const auto path=xml->getStringAttribute("file");
        if(path.isNotEmpty()) {
            if(!juce::File::isAbsolutePath(path)) return juce::Result::fail("Invalid instrument path: "+path);
            if(!juce::File(path).existsAsFile()) { missing=path; return juce::Result::fail("Instrument file not found"); }
        }
        for(auto* entry=xml->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("MapEdit")) {
            if(entry->getBoolAttribute("clear",false)&&entry->getStringAttribute("path").isEmpty()) continue;
            const auto mapped=entry->getStringAttribute("path");
            if(!juce::File::isAbsolutePath(mapped)) return juce::Result::fail("Invalid instrument path: "+mapped);
            if(!juce::File(mapped).existsAsFile()) {missing=mapped; return juce::Result::fail("Instrument file not found");}
        }
        xml->setAttribute("presetFile",f.getRelativePathFrom(presetRoot())); copyXmlToBinary(*xml,state); return juce::Result::ok();
    }
    void loadFile(const juce::File& f) {
        setCurrentPresetFile(juce::File{});
        if(f.hasFileExtension("sfz")) setUiChannel(0);
        if(f.hasFileExtension("sf2;sf3") && midiFX->getIndex()==1 && effects->getIndex()==0)
            effects->setValueNotifyingHost(effects->convertTo0to1(3.0f));
        auto p=initialPrograms(f.hasFileExtension("sfz"));
        load(f.getFullPathName(),p,emptyControls(),{},0,false,InstrumentGains{},-1,-1,lsfx::NoteAssignments{});
    }
    juce::AudioParameterFloat& gainParameter() { return *gain; }
    juce::AudioParameterFloat& channelGainParameter(int ch) { return *channelGain[static_cast<size_t>(ch)]; }
    juce::AudioParameterChoice& routeParameter(int ch) { return *channelRoute[static_cast<size_t>(ch)]; }
    juce::RangedAudioParameter& compressorParameter(int ch,int i) {return *compParams[ch][i];}
    float compressorValue(int ch,int i) const {auto* p=compParams[ch][i];return p->convertFrom0to1(p->getValue());}
    juce::AudioParameterChoice& qualityParameter() { return *quality; }
    juce::AudioParameterChoice& effectsParameter() { return *effects; }
    juce::AudioParameterChoice& midiFXParameter() { return *midiFX; }
    juce::AudioParameterChoice& sfzQualityParameter() { return *sfzQuality; }
    juce::RangedAudioParameter& fxParameter(int i) { return *fxParams[i]; }
    juce::String diagnostic() const { return "Note route pool misses: "+juce::String(noteRouteOverflow.load())+"; Active voices: "+juce::String(activeVoices.load())+"; SF2 idle sleep: "+(sleeping.load()?juce::String("yes"):juce::String("no"))+"; dropped engine-swap blocks: "+juce::String(droppedBlocks.load())+"; MIDI program changes: "+juce::String(programChanges.load())+"; last program change: "+lastProgramText()+"; CH 10 CC7: "+juce::String(getControl(9,7))+"; CH 10 CC11: "+juce::String(getControl(9,11))+"; CH 10 bank/program: "+juce::String(current(9)>>7)+"/"+juce::String(current(9)&127)+"; CH 10 reverb send: "+juce::String(getControl(9,91))+"; CH 10 chorus send: "+juce::String(getControl(9,93))+"; MIDI CC91: "+juce::String(reverbCC.load())+"; MIDI CC93: "+juce::String(chorusCC.load())+"; SysEx received: "+juce::String(sysexCount.load())+"; FX SysEx applied: "+juce::String(fxSysexCount.load()); }
    int getControl(int ch,int id) const {
        const int i=controlIndex(ch,id), pendingValue=controlCommands[i].load();
        return pendingValue==unset?controlValues[i].load():pendingValue;
    }
    void setControl(int ch,int id,int value) {
        if(ch<0||ch>=channels||id<0||id>=slots) return;
        if(id<128) { if(id==0||id==32||id>=120) return; value=juce::jlimit(0,127,value); }
        else if(auto* spec=sf2Spec(id)) value=juce::jlimit(spec->lo,spec->hi,value); else return;
        std::lock_guard<std::mutex> lock(requestMutex);
        if(pending||loadingSerial!=0||failedSerial==latest.serial||(latest.path.isEmpty()&&latest.edits.empty())) return;
        const int i=controlIndex(ch,id); overrides[i]=value;
        controlCommands[i].store(value); controlDirty.store(true);
    }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        b.clear();
        if(b.getNumSamples()==0) { midi.clear(); return; }
        // Exclusive dry editor audition, also usable when the main map is empty.
        if(instrumentPreview.render(b,midi))return;
        // No waiting for disk I/O or for the worker on the audio thread.
        std::unique_lock<std::mutex> lock(engineMutex, std::try_to_lock);
        if (!lock.owns_lock()) { ++droppedBlocks; for(const auto m:midi) if(m.numBytes>0&&m.data[0]<0xf8) { panic.store(true); break; } midi.clear(); return; }
        if (clearing.load() || !engine || std::abs(engine->rate-hostRate.load())>0.01 || b.getNumChannels()<2) { activeVoices.store(0); sleeping.store(false); midi.clear(); return; }
        for(int ch=0;ch<channels;++ch) engine->channelGains[ch].setTargetValue(juce::Decibels::decibelsToGain(channelGain[ch]->get(),channelGainMin-1.0f));
        for(int ch=0;ch<channels;++ch) {
            lsfx::CompValues values{};
            for(int i=0;i<lsfx::compCount;++i) values[i]=compressorValue(ch,i);
            engine->compressors[ch].configure(values);
        }
        bool routingChanged=false;
        for(int ch=0;ch<channels;++ch) {
            const int next=(engine->sfzActive[ch]||sf2MultiOutput)?channelRoute[ch]->getIndex():0;
            routingChanged=routingChanged||engine->routes[ch]!=next;
            engine->routes[ch]=next;
        }
        std::array<int,stereoBuses> dest{}; dest.fill(-1);
        for(int bus=0;bus<stereoBuses;++bus) if(auto* outputBus=getBus(false,bus)) {
            if(outputBus->isEnabled()&&outputBus->getNumberOfChannels()==2) {
                const int index=getChannelIndexInProcessBlockBuffer(false,bus,0);
                if(index>=0&&index+1<b.getNumChannels()) dest[bus]=index;
            }
        }
        auto f=readFX(); int fx=effects->getIndex();
        const int followMode=midiFX->getIndex();
        if(engine->lastMidiFxMode!=followMode) {engine->midiDisabledFx=0; engine->lastMidiFxMode=followMode;}
        // Apply before this block's MIDI events, so the new song can send its
        // own Bank Select / Program Change / CCs immediately after the reset.
        const bool midiReset=midiResetRequested.exchange(false);
        if(midiReset) resetLiveMidi(*engine,f,fx);
        const bool settingsChanged=configureFX(*engine,f,fx);
        auto* s = engine->synth;
        const int q=quality->getIndex();
        bool controlChanged = midiReset || q!=engine->quality || settingsChanged || routingChanged;
        if(applyControls(*engine)) controlChanged=true;
        if(engine->hasSF2&&updateSF2Trims(*engine)) controlChanged=true;
        if(engine->hasSF2&&applyInitialAmbience(*engine,f[sf2Ambience])) controlChanged=true;
        if(q!=engine->quality) { const int methods[]={FLUID_INTERP_LINEAR,FLUID_INTERP_4THORDER,FLUID_INTERP_7THORDER}; fluid_synth_set_interp_method(s,-1,methods[q]); engine->quality=q; }
        if (panic.exchange(false)) { controlChanged=true; engine->resetFX(); for (int ch=0;ch<channels;++ch) { if(auto* z=engine->sfzActive[ch]) z->stop(); engine->sfzCC[controlIndex(ch,64)]=0; fluid_synth_cc(s,ch,64,0); fluid_synth_cc(s,ch,66,0); controlValues[controlIndex(ch,64)].store(0); controlValues[controlIndex(ch,66)].store(0); fluid_synth_all_sounds_off(s,ch); } }
        for (int ch=0; ch<channels; ++ch) {
            const int cmd = commands[ch].exchange(-1);
            if (cmd>=0) { controlChanged=true; selectLive(*engine,ch,cmd); }
            const int retry=sfzRetry[ch].exchange(-1);
            if(retry>=0&&retry==selected[ch].load()&&!engine->sfzActive[ch]) { controlChanged=true; selectLive(*engine,ch,retry); }
        }
        // A manual program change must also retain the user trim.
        if(controlChanged&&engine->hasSF2) updateSF2Trims(*engine);
        if constexpr (sf2MultiOutput) { if(updateRouteSends(*engine)) controlChanged=true; }
        // MIDI clock / sensing do not need to wake the synthesizer.
        bool relevantMidi=false;
        for(const auto m:midi) if(m.numBytes>0&&m.data[0]<0xf8) { relevantMidi=true; break; }
        const int before=fluid_synth_get_active_voice_count(s);
        const int sleepAfter=int(engine->rate*2.0);
        if(before==0&&engine->quietSamples>=sleepAfter) engine->noteGroups.reset();
        if(!engine->hasSFZ()&&!controlChanged&&!relevantMidi&&before==0&&engine->quietSamples>=sleepAfter) { activeVoices.store(0); sleeping.store(true); midi.clear(); return; }
        sleeping.store(false);
        if(controlChanged||relevantMidi||before>0) engine->quietSamples=0;
        // FluidSynth renders each section preceding the corresponding MIDI event.
        // Its internal block size may still quantise event timing slightly.
        float rawPeak=0, wetPeak=0;
        auto render = [&](int start,int count) {
            if(count<=0) return;
            if constexpr (!sf2MultiOutput) {
                // TEST14: use the original TEST11 single-stereo render path.
                // All sixteen MIDI channels and the native FX are mixed by FluidSynth.
                // Optional channel trim is applied to voice attenuation before mixing.
                if(engine->hasSF2&&fluid_synth_write_float(s,count,b.getWritePointer(0,start),0,1,b.getWritePointer(1,start),0,1)!=FLUID_OK) {
                    b.clear(0,start,count); b.clear(1,start,count);
                    return;
                }
                if(!engine->hasSF2) {b.clear(0,start,count); b.clear(1,start,count);}
                if(engine->hasSFZ()) renderSfz(*engine,b,dest,f,fx,start,count);
                rawPeak=juce::jmax(rawPeak,juce::jmax(b.getMagnitude(0,start,count),b.getMagnitude(1,start,count)));
                wetPeak=juce::jmax(wetPeak,processMaster(*engine,b,f,fx,start,count));
            } else {
                for(int offset=0;offset<count;) {
                    const int n=juce::jmin(renderChunk,count-offset), pos=start+offset;
                    std::array<float*,2*sf2Groups> dryPtrs{};
                    std::array<float*,4> wetPtrs{};
                    for(int i=0;i<2*engine->noteGroups.used;++i) { dryPtrs[size_t(i)]=engine->dry[size_t(i)].data(); std::fill_n(dryPtrs[size_t(i)],n,0.0f); }
                    for(size_t i=0;i<wetPtrs.size();++i) { wetPtrs[i]=engine->wet[i].data(); std::fill_n(wetPtrs[i],n,0.0f); }
                    if(engine->hasSF2&&fluid_synth_process(s,n,int(wetPtrs.size()),wetPtrs.data(),int(dryPtrs.size()),dryPtrs.data())!=FLUID_OK) { offset+=n; continue; }
                    if(engine->hasSF2) for(int ch=0;ch<channels;++ch) {
                        if(engine->sfzActive[ch]) continue;
                        std::array<float*,sf2Groups> left{},right{};
                        std::array<int,sf2Groups> routes{};
                        int pairs=1;left[0]=dryPtrs[2*ch];right[0]=dryPtrs[2*ch+1];routes[0]=engine->routes[ch];
                        for(int group=channels;group<engine->noteGroups.used;++group) if(engine->noteGroups.owner[group]==ch) {
                            left[pairs]=dryPtrs[2*group];right[pairs]=dryPtrs[2*group+1];routes[pairs]=engine->noteGroups.destination[group];++pairs;
                        }
                        engine->compressors[ch].processSplit(left.data(),right.data(),pairs,n);
                        for(int pair=0;pair<pairs;++pair) {
                            const int index=dest[routes[pair]];
                            if(index<0) continue;
                            b.addFrom(index,pos,left[pair],n);b.addFrom(index+1,pos,right[pair],n);
                        }
                    }
                    // One shared native reverb/chorus group; only Main receives returns.
                    if(engine->hasSF2) for(int i=0;i<4;++i) b.addFrom(i%2,pos,wetPtrs[i],n);
                    if(engine->hasSFZ()) renderSfz(*engine,b,dest,f,fx,pos,n);
                    rawPeak=juce::jmax(rawPeak,juce::jmax(b.getMagnitude(0,pos,n),b.getMagnitude(1,pos,n)));
                    wetPeak=juce::jmax(wetPeak,processMaster(*engine,b,f,fx,pos,n));
                    offset+=n;
                }
            }
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
                bool resetMap=false;
                lsfx::decodeMidiEffects(d,n,[&](lsfx::MidiFx k,int,int,int){if(k==lsfx::MidiFx::reset) resetMap=true;});
                for(int c=0;c<channels;++c) {
                    if(resetMap) {const int code=defaults()[c];selectLive(*engine,c,code);}
                    if(!engine->sfzActive[c]) refreshFluidControls(s,c);
                }
                applyMidiSysEx(*engine,d,n,f,fx);
                updateSF2Trims(*engine);
                if constexpr (sf2MultiOutput) updateRouteSends(*engine);
            } else if(status<0xf0 && status>=0x80 && n>=2) {
                const int a=d[1]&127, v=n>=3 ? d[2]&127 : 0;
                if constexpr (sf2MultiOutput) { if((status&0xf0)==0x90) updateRouteSend(*engine,ch); }
                switch(status&0xf0) {
                    case 0x80: if(n>=3) { if(auto* z=engine->sfzActive[ch]) sfizz_send_note_off(z->synth,0,a,v); else fluid_synth_noteoff(s,ch,a); } break;
                    case 0x90: if(n>=3) {
                        if(v&&noteLearnChannel.load(std::memory_order_relaxed)==ch) {
                            const uint64_t address=static_cast<uint64_t>(selected[ch].load());
                            learnedNote.store((uint64_t(++noteEventSerial)<<32)|(address<<7)|uint64_t(a),std::memory_order_relaxed);
                        }
                        if(auto* z=engine->sfzActive[ch]) { if(v) sfizz_send_note_on(z->synth,0,a,v); else sfizz_send_note_off(z->synth,0,a,0); }
                        else if(const auto* preset=findPreset(engine->map,selected[ch].load());preset&&preset->sfzIndex<0) {
                            if(!v) fluid_synth_noteoff(s,ch,a);
                            else if constexpr (sf2MultiOutput) {
                                const auto assignment=lsfx::noteAssignment(noteAssignments,selected[ch].load(),a);
                                const int group=assignment.output<0?ch:engine->noteGroups.acquire(ch,assignment.output);
                                if(group<0) {++noteRouteOverflow;} // do not send an explicitly routed note to the wrong bus
                                else lsfx_fluid_noteon(s,ch,a,v,group,assignment.output<0?0:assignment.output==0?1:2,
                                    juce::Decibels::decibelsToGain(float(assignment.level)*.1f),float(assignment.pan)*5.0f,float(assignment.pitch));
                            } else fluid_synth_noteon(s,ch,a,v);
                        }
                    } break;
                    case 0xa0: if(n>=3) {if(auto* z=engine->sfzActive[ch]) sfizz_send_poly_aftertouch(z->synth,0,a,v); else fluid_synth_key_pressure(s,ch,a,v);} break;
                    case 0xb0: if(n>=3) { receiveFluidCC(*engine,ch,a,v,f,fx); if(a==121||a==6||a==38||a==96||a==97) { if(!engine->sfzActive[ch]) refreshFluidControls(s,ch); updateSF2Trim(*engine,ch); } if constexpr (sf2MultiOutput) updateRouteSend(*engine,ch); } break;
                    case 0xc0: { receiveProgram(ch,a); int font=0,bank=selected[ch].load()>>7,program=0;
                        fluid_synth_get_program(s,ch,&font,&bank,&program);
                        const int code=pack(juce::jlimit(0,16383,bank),a);
                        selectLive(*engine,ch,code); updateSF2Trim(*engine,ch);
                        if constexpr (sf2MultiOutput) updateRouteSend(*engine,ch); break; }
                    case 0xd0: if(auto* z=engine->sfzActive[ch]) sfizz_send_channel_aftertouch(z->synth,0,a); else fluid_synth_channel_pressure(s,ch,a); break;
                    case 0xe0: if(n>=3) { fluid_synth_pitch_bend(s,ch,a|(v<<7)); if(auto* z=engine->sfzActive[ch]) sfizz_send_hd_pitch_wheel(z->synth,0,((a|(v<<7))-8192)/8192.0f); } break;
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
        int after=fluid_synth_get_active_voice_count(s); for(auto* z:engine->sfzActive) if(z) after+=sfizz_get_num_active_voices(z->synth); activeVoices.store(after);
        if(after==0&&before==0&&!relevantMidi&&!controlChanged&&rawPeak<0.00001f&&wetPeak<0.00001f)
            engine->quietSamples=juce::jmin(sleepAfter,engine->quietSamples+b.getNumSamples());
        else engine->quietSamples=0;
        midi.clear();
    }
    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::String path; Programs p; Controls c; MapEdits edits; int sfzCode=0; InstrumentGains savedGains; lsfx::NoteAssignments savedNotes; stateForSave(path,p,&c,&edits,&sfzCode,&savedGains,&savedNotes);
        juce::XmlElement x("LSoundFontState");
        x.setAttribute("uiChannel",uiChannel());
        x.setAttribute("version",14); x.setAttribute("sfzCode",sfzCode); x.setAttribute("midiFX",midiFX->getIndex()); for(int i=0;i<fxCount;++i) x.setAttribute(fxSpecs[i].key,i==drumTrim?0:readFXValue(i)); x.setAttribute("sfzQuality",sfzQuality->getIndex()); x.setAttribute("quality",quality->getIndex()); x.setAttribute("effects",effects->getIndex()); x.setAttribute("file",path); x.setAttribute("gain",static_cast<double>(gain->get()));
        for(int i=0;i<channels;++i) { x.setAttribute("ch"+juce::String(i+1),p[i]); x.setAttribute(channelGainId(i),double(channelGain[i]->get())); x.setAttribute(routeId(i),channelRoute[i]->getIndex()); }
        for(int ch=0;ch<channels;++ch) for(int i=0;i<lsfx::compCount;++i) x.setAttribute(compressorId(ch,i),double(compressorValue(ch,i)));
        for(int i=0;i<controlCount;++i) if(c[i]!=unset) {
            auto* entry=x.createNewChildElement("Control"); entry->setAttribute("index",i); entry->setAttribute("value",c[i]);
        }
        for(const auto& e:edits) {auto* entry=x.createNewChildElement("MapEdit"); entry->setAttribute("bank",e.bank);entry->setAttribute("program",e.program);entry->setAttribute("sourceBank",e.sourceBank);entry->setAttribute("sourceProgram",e.sourceProgram);entry->setAttribute("path",e.path);if(e.clear)entry->setAttribute("clear",true);}
        for(const auto& item:savedGains) {auto* entry=x.createNewChildElement("InstrumentGain"); entry->setAttribute("address",item.first); entry->setAttribute("db",double(item.second));}
        for(const auto& program:savedNotes) for(const auto& item:program.second) {
            const auto& v=item.second;auto* entry=x.createNewChildElement("NoteAssignment");
            entry->setAttribute("address",program.first);entry->setAttribute("note",item.first);entry->setAttribute("output",v.output);
            entry->setAttribute("level",v.level);entry->setAttribute("pan",v.pan);entry->setAttribute("pitch",v.pitch);
        }
        const auto preset=currentPresetFile();
        if(preset!=juce::File{}&&preset.isAChildOf(presetRoot())) x.setAttribute("presetFile",preset.getRelativePathFrom(presetRoot()));
        copyXmlToBinary(x,dest);
    }
    void setStateInformation(const void* data,int size) override {
        if(auto x=getXmlFromBinary(data,size)) if(x->hasTagName("LSoundFontState")) {
            const auto relative=x->getStringAttribute("presetFile");
            const auto f=relative.isNotEmpty()?presetRoot().getChildFile(relative):juce::File{};
            setCurrentPresetFile(f!=juce::File{}&&f.isAChildOf(presetRoot())&&f.hasFileExtension("lsfx")?f:juce::File{});
            setUiChannel(x->getIntAttribute("uiChannel",0));
            auto p=defaults();
            for(int i=0;i<channels;++i) p[i]=juce::jlimit(0,pack(16383,127),x->getIntAttribute("ch"+juce::String(i+1),p[i]));
            midiFX->setValueNotifyingHost(midiFX->convertTo0to1(float(juce::jlimit(0,1,x->getIntAttribute("midiFX",1)))));
            gain->setValueNotifyingHost(gain->convertTo0to1(static_cast<float>(x->getDoubleAttribute("gain",0.0))));
            for(int ch=0;ch<channels;++ch) {
                const auto v=static_cast<float>(x->getDoubleAttribute(channelGainId(ch),0.0));
                auto& a=*channelGain[ch]; a.setValueNotifyingHost(a.convertTo0to1(std::isfinite(v)?juce::jlimit(channelGainMin,channelGainMax,v):0.0f));
                auto& routing=*channelRoute[ch]; routing.setValueNotifyingHost(routing.convertTo0to1(float(juce::jlimit(0,stereoBuses-1,x->getIntAttribute(routeId(ch),0)))));
            }
            for(int ch=0;ch<channels;++ch) for(int i=0;i<lsfx::compCount;++i) {
                auto& par=*compParams[ch][i]; const auto& spec=lsfx::compSpecs[i];
                const auto db=x->getDoubleAttribute(compressorId(ch,i),spec.initial);
                const float v=std::isfinite(db)?juce::jlimit(spec.lo,spec.hi,float(db)):spec.initial;
                par.setValueNotifyingHost(par.convertTo0to1(v));
            }
            quality->setValueNotifyingHost(quality->convertTo0to1(static_cast<float>(juce::jlimit(0,2,x->getIntAttribute("quality",1)))));
            effects->setValueNotifyingHost(effects->convertTo0to1(static_cast<float>(juce::jlimit(0,3,x->getIntAttribute("effects",0)))));
            sfzQuality->setValueNotifyingHost(sfzQuality->convertTo0to1(static_cast<float>(juce::jlimit(0,11,x->getIntAttribute("sfzQuality",0)))));
            for(int i=0;i<fxCount;++i) { const auto& f=fxSpecs[i]; fxParams[i]->setValueNotifyingHost(fxParams[i]->convertTo0to1(float(i==drumTrim?0:juce::jlimit(f.lo,f.hi,x->getIntAttribute(f.key,f.initial))))); }
            auto c=emptyControls();
            for(auto* entry=x->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("Control")) {
                const int i=entry->getIntAttribute("index",-1), v=entry->getIntAttribute("value",unset);
                if(i<0||i>=controlCount||v==unset) continue;
                const int id=i%slots;
                if(id<120&&id!=0&&id!=32) c[i]=juce::jlimit(0,127,v);
                else if(id>=128) if(auto* spec=sf2Spec(id)) c[i]=juce::jlimit(spec->lo,spec->hi,v);
            }
            MapEdits edits;
            for(auto* entry=x->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("MapEdit")) {
                MapEdit e{entry->getIntAttribute("bank",-1),entry->getIntAttribute("program",-2),entry->getIntAttribute("sourceBank",-1),entry->getIntAttribute("sourceProgram",-1),entry->getStringAttribute("path")};
                e.clear=entry->getBoolAttribute("clear",false);
                if(e.program>=-1&&lsfx::validAddress(e.bank,e.program<0?0:e.program)) {
                    if(e.clear&&e.path.isEmpty()) edits.push_back(std::move(e));
                    else if(!e.clear&&lsfx::validAddress(e.sourceBank,e.sourceProgram)&&juce::File::isAbsolutePath(e.path)&&(juce::File(e.path).hasFileExtension("sf2;sf3")||(e.program>=0&&juce::File(e.path).hasFileExtension("sfz")))) edits.push_back(std::move(e));
                }
            }
            const auto basePath=x->getStringAttribute("file");
            const int baseCode=juce::jlimit(0,pack(16383,127),x->getIntAttribute("sfzCode",0));
            if(x->getIntAttribute("version",0)<11&&juce::File(basePath).hasFileExtension("sfz")&&edits.empty()) {
                for(int ch=1;ch<channels;++ch) if(p[ch]==baseCode) p[ch]=pack(baseCode>>7,(baseCode+1)&127);
            }
            InstrumentGains restored;
            for(auto* entry=x->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("InstrumentGain")) {
                const int code=entry->getIntAttribute("address",-1); const auto db=entry->getDoubleAttribute("db",0.0);
                if(code>=0&&code<=pack(16383,127)&&std::isfinite(db)&&db!=0.0) restored[code]=juce::jlimit(sfzGainMin,sfzGainMax,float(db));
            }
            lsfx::NoteAssignments restoredNotes;
            for(auto* entry=x->getFirstChildElement();entry;entry=entry->getNextElement()) if(entry->hasTagName("NoteAssignment")) {
                lsfx::setNoteAssignment(restoredNotes,entry->getIntAttribute("address",-1),entry->getIntAttribute("note",-1),
                    {entry->getIntAttribute("output",-1),entry->getIntAttribute("level",0),entry->getIntAttribute("pan",0),entry->getIntAttribute("pitch",0)});
            }
            load(basePath,p,c,std::move(edits),baseCode,false,std::move(restored),-1,-1,std::move(restoredNotes));
        }
    }
private:
    std::atomic<int> editorChannel{0};
    std::mutex presetMutex;
    juce::File currentPreset;
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
        if(cc!=0&&cc!=32) {
            if(cc<120) e.sfzCC[controlIndex(ch,cc)]=v;
            if(auto* z=e.sfzActive[ch]) sfizz_send_cc(z->synth,0,cc,v);
        }
        if(cc==91) e.reverbSendExplicit[ch]=true;
        if(cc<120) controlValues[controlIndex(ch,cc)].store(v);
        if(cc==91||cc==93) {
            if(cc==91) ++reverbCC; else ++chorusCC;
            const int bit=cc==91?1:2;
            if(e.hasSF2&&!e.sfzActive[ch]&&e.routes[ch]==0 && v>0 && midiFX->getIndex()==1 && !(e.midiDisabledFx&bit) && !(fx&bit)) {
                midiEffectMask(fx|bit,fx); configureFX(e,f,fx);
            }
        }
    }
    bool applyInitialAmbience(Engine& e,int profile) {
        if(e.ambienceApplied==profile) return false;
        e.ambienceApplied=profile;
        for(int ch=0;ch<channels;++ch) if(!e.reverbSendExplicit[ch]) {
            const int send=profile==1?40:0;
            fluid_synth_cc(e.synth,ch,91,send);
            if(!e.sfzActive[ch]) controlValues[controlIndex(ch,91)].store(send);
        }
        return true;
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
            if(k==K::send||k==K::partVolume) { receiveFluidCC(e,ch,cc,v,f,fx); applied=true; return; }
            if(k==K::reset) {
                e.midiMaster=1.0f; e.midiDisabledFx=0;
                e.reverbSendExplicit.fill(false); e.ambienceApplied=-1;
                // Reset defaults never override subsequent MIDI CC91, including zero.
                applyInitialAmbience(e,f[sf2Ambience]);
            }
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
        if(old[chType]!=f[chType]||((e.effects^enabled)&2)) {e.modulation.reset(); e.sfzModulation.reset();}
        if(old[delayMode]!=f[delayMode]||(old[delayMix]==0)!=(f[delayMix]==0)) e.delay.reset();
        if(e.hasSF2) {
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
        }
        if(e.hasSFZ() && (old[rvType]!=f[rvType]||old[rvLevel]!=f[rvLevel]||old[rvTime]!=f[rvTime]||old[rvDamp]!=f[rvDamp]||old[rvWidth]!=f[rvWidth]||e.effects!=enabled)) {
            juce::Reverb::Parameters p;
            p.roomSize=roomSize(f); p.damping=f[rvDamp]*.01f; p.width=f[rvWidth]*.01f;
            p.wetLevel=f[rvLevel]*.0035f; p.dryLevel=0.5f; p.freezeMode=0;
            e.sfzReverb.setParameters(p);
        }
        e.effects=enabled; e.lastFx=f; return true;
    }
    float processMaster(Engine& e,juce::AudioBuffer<float>& b,const FXValues& f,int enabled,int start=0,int count=-1) {
        const bool reverbEcho=(enabled&1)&&f[rvType]>=6;
        const bool chorusCustom=(enabled&2)&&(!e.hasSF2||f[chType]>=4);
        const bool extraDelay=f[delayMix]>0;
        auto* l=b.getWritePointer(0,start); auto* r=b.getWritePointer(1,start); const int n=count<0?b.getNumSamples()-start:count;
        if(!e.hasSF2&&e.hasSFZ()&&(enabled&1)&&f[rvType]<6) e.sfzReverb.processStereo(l,r,n);
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
            if(e.hasSF2) e.output.processSoundFont(l[i],r[i],g,trim,f[sf2Dynamics]);
            else e.output.process(l[i],r[i],g,trim);
        }
        return peak;
    }
    void renderSfz(Engine& e,juce::AudioBuffer<float>& b,const std::array<int,stereoBuses>& dest,const FXValues& f,int fx,int start,int count) {
        const int sq=sfzQuality->getIndex();
        for(int offset=0;offset<count;) {
            const int n=juce::jmin(renderChunk,count-offset),pos=start+offset;
            std::fill_n(e.sfzMainL.data(),n,0.0f); std::fill_n(e.sfzMainR.data(),n,0.0f);
            for(int ch=0;ch<channels;++ch) if(auto* slot=e.sfzActive[ch]) {
                if(slot->appliedQuality!=sq) {
                    if(sq!=0||slot->appliedQuality!=-1) sfizz_set_sample_quality(slot->synth,SFIZZ_PROCESS_LIVE,sq==0?slot->defaultQuality:sq-1);
                    slot->appliedQuality=sq;
                }
                float* out[]={e.sfzL.data(),e.sfzR.data()};
                std::fill_n(out[0],n,0.0f); std::fill_n(out[1],n,0.0f);
                sfizz_render_block(slot->synth,out,2,n);
                auto& g=e.channelGains[ch];
                const auto it=instrumentGains.find(e.sfzAddress[ch]);
                const float target=juce::Decibels::decibelsToGain(it==instrumentGains.end()?0.0f:it->second,sfzGainMin-1.0f);
                if(!slot->gainPrimed) {slot->instrumentGain.reset(e.rate,0.01); slot->instrumentGain.setCurrentAndTargetValue(target); slot->gainPrimed=true;}
                else slot->instrumentGain.setTargetValue(target);
                for(int i=0;i<n;++i) {const float v=g.getNextValue()*slot->instrumentGain.getNextValue();out[0][i]*=v;out[1][i]*=v;}
                e.compressors[ch].process(out[0],out[1],n);
                const int route=channelRoute[ch]->getIndex();
                if(route==0) for(int i=0;i<n;++i) {e.sfzMainL[i]+=out[0][i];e.sfzMainR[i]+=out[1][i];}
                else if(dest[route]>=0) {b.addFrom(dest[route],pos,out[0],n); b.addFrom(dest[route]+1,pos,out[1],n);}
            }
            // FluidSynth retains its native shared effects. Apply the existing
            // SFZ shared reverb/chorus only to the SFZ Main submix in hybrid mode.
            if(e.hasSF2) {
                if((fx&1)&&f[rvType]<6) e.sfzReverb.processStereo(e.sfzMainL.data(),e.sfzMainR.data(),n);
                if((fx&2)&&f[chType]<4) for(int i=0;i<n;++i) {
                    float wl=0,wr=0;
                    e.sfzModulation.process(e.sfzMainL[i],e.sfzMainR[i],14.0f,chorusDepthMs(f),f[chRate]*.01f,0,false,wl,wr);
                    e.sfzMainL[i]+=wl*f[chLevel]*.01f; e.sfzMainR[i]+=wr*f[chLevel]*.01f;
                }
            }
            b.addFrom(0,pos,e.sfzMainL.data(),n); b.addFrom(1,pos,e.sfzMainR.data(),n);
            offset+=n;
        }
    }
    juce::AudioParameterFloat* gain = nullptr;
    std::array<juce::AudioParameterFloat*,channels> channelGain{};
    std::array<juce::AudioParameterChoice*,channels> channelRoute{};
    std::array<std::array<juce::RangedAudioParameter*,lsfx::compCount>,channels> compParams{};
    static bool updateRouteSend(Engine& e,int ch) {
        return lsfx_fluid_channel_dry(e.synth,ch,e.routes[ch]!=0?1:0)>0;
    }
    static bool updateRouteSends(Engine& e) {
        bool changed=false; for(int ch=0;ch<channels;++ch) changed=updateRouteSend(e,ch)||changed; return changed;
    }
    juce::AudioParameterChoice* quality = nullptr;
    juce::AudioParameterChoice* effects = nullptr;
    juce::AudioParameterChoice* midiFX = nullptr;
    juce::AudioParameterChoice* sfzQuality = nullptr;
    std::array<std::atomic<int>,controlCount> controlValues{}, controlCommands{};
    std::atomic<bool> controlDirty{false};
    Controls overrides=emptyControls(); // protected by requestMutex
    static void sendControl(Engine& e,int ch,int id,int value) {
        if(id<120&&id!=0&&id!=32) {
            e.sfzCC[controlIndex(ch,id)]=value;
            if(auto* z=e.sfzActive[ch]) sfizz_send_cc(z->synth,0,id,value);
            fluid_synth_cc(e.synth,ch,id,value); if(id==91) e.reverbSendExplicit[ch]=true;
        } else if(id>=128) if(auto* spec=sf2Spec(id)) fluid_synth_set_gen(e.synth,ch,spec->gen,static_cast<float>(value));
    }
    bool applyControls(Engine& e) {
        if(!controlDirty.exchange(false)) return false;
        for(int i=0;i<controlCount;++i) {
            const int v=controlCommands[i].exchange(unset);
            if(v!=unset) { sendControl(e,i/slots,i%slots,v); controlValues[i].store(v); }
        }
        return true;
    }
    bool updateSF2Trim(Engine& e,int ch) {
        return e.sf2Trims[ch].update(channelGain[ch]->get(),
            [&]{return fluid_synth_get_gen(e.synth,ch,GEN_ATTENUATION);},
            [&](float value){return fluid_synth_set_gen(e.synth,ch,GEN_ATTENUATION,value)==FLUID_OK;});
    }
    bool updateSF2Trims(Engine& e) {
        bool changed=false;
        for(int ch=0;ch<channels;++ch) changed=updateSF2Trim(e,ch)||changed;
        return changed;
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
    std::array<std::atomic<int>,channels> sfzNeeded{},sfzRetry{};
    std::atomic<bool> extraSfzPending{false};
    std::atomic<bool> panic{false}, clearing{false}, midiResetRequested{false};
    std::mutex engineMutex, requestMutex, viewMutex;
    InstrumentGains instrumentGains; // protected by engineMutex, already held during render
    std::mutex noteMutex;
    lsfx::NoteAssignments noteAssignments;
    std::atomic<int> noteLearnChannel{-1};
    std::atomic<int> noteRouteOverflow{0};
    std::atomic<uint64_t> learnedNote{0};
    uint32_t noteEventSerial=0; // written only by processBlock
    std::condition_variable cv;
    std::unique_ptr<Engine> engine;
    std::thread worker;
    bool stopping=false, pending=false;
    Request latest;
    std::optional<Request> importRollback;
    bool rollbackWasFailed=false;
    View view;
    static bool choose(Engine& e,int ch,int code) {
        const auto* preset=findPreset(e.map,code);
        const bool sfz=preset&&preset->sfzIndex>=0;
        if(!preset||sfz||fluid_synth_program_select(e.synth,ch,preset->font,preset->sourceBank,preset->sourceProgram)!=FLUID_OK) {
            fluid_synth_all_sounds_off(e.synth,ch);
            fluid_synth_unset_program(e.synth,ch);
        }
        const bool ready=e.activateSfz(ch,code,preset);
        // Destination bank must remain independent of original SF2 bank.
        fluid_synth_bank_select(e.synth,ch,code>>7);
        return ready;
    }
    void readSfzControls(Engine& e,int ch,const Preset& preset) {
        for(int cc=1;cc<120;++cc) if(cc!=32) controlValues[controlIndex(ch,cc)].store(e.sfzCC[controlIndex(ch,cc)]);
        for(const auto& cc:e.sfzSources[static_cast<size_t>(preset.sfzIndex)].labels) {
            const int v=e.sfzCC[controlIndex(ch,cc.cc)];
            controlValues[controlIndex(ch,cc.cc)].store(v==unset?cc.initial:v);
        }
    }
    void resetLiveMidi(Engine& e,FXValues& f,int& fx) {
        // Native system reset clears channel modes, notes, controllers, bank
        // state, pitch bend, pressure, RPN/NRPN and generator offsets. Rebind
        // virtual addresses explicitly: FluidSynth does not know our map.
        fluid_synth_system_reset(e.synth);
        for(int ch=0;ch<channels;++ch) {
            if(auto* slot=e.sfzActive[ch]) {
                sfizz_all_sound_off(slot->synth);
                slot->owner=-1; e.sfzActive[ch]=nullptr;
            }
            e.sfzAddress[ch]=-1;
            sfzNeeded[ch].store(-1); sfzRetry[ch].store(-1);
        }
        e.sfzCC=emptyControls();
        for(auto& c:controlValues) c.store(unset);
        e.sf2Trims={};
        e.midiMaster=1.0f; e.midiDisabledFx=0;
        e.reverbSendExplicit.fill(false); e.ambienceApplied=-1;
        // Follow MIDI exposes MIDI-driven effect settings through host
        // parameters. Restore their initial profile, but leave manual FX
        // and all mixer-specific settings (gain, compressors, routes) alone.
        if(midiFX->getIndex()==1) {
            for(int i=0;i<inputTrim;++i) {
                f[i]=fxSpecs[i].initial;
                auto* par=fxParams[i];
                static_cast<juce::AudioProcessorParameter*>(par)->setValue(par->convertTo0to1(float(f[i])));
            }
            midiEffectMask(e.hasSF2?3:0,fx);
        }
        e.resetFX(); e.lastFx.fill(-1); e.effects=-1; e.quality=-1;
        for(int ch=0;ch<channels;++ch) selectLive(e,ch,e.resetPrograms[ch]);
        // Do not recycle note-route groups until the existing render-path
        // silence check: FluidSynth may still hold a partially rendered block.
    }
    void selectLive(Engine& e,int ch,int code) {
        const bool ready=choose(e,ch,code); selected[ch].store(code);
        sfzNeeded[ch].store(ready?-1:code);
        if(!ready) { extraSfzPending.store(true); cv.notify_one(); }
        const auto* preset=findPreset(e.map,code);
        if(preset&&preset->sfzIndex>=0) readSfzControls(e,ch,*preset);
        else refreshFluidControls(e.synth,ch);
        e.routes[ch]=((preset&&preset->sfzIndex>=0)||sf2MultiOutput)?channelRoute[ch]->getIndex():0;
    }
    // Worker-only extra copies when several channels select the same SFZ.
    void loadExtraSfz() {
        for(int ch=0;ch<channels;++ch) {
            const int code=sfzNeeded[ch].exchange(-1);
            if(code<0) continue;
            juce::String path; double rate=0; uint64_t serial=0; int index=-1;
            { std::lock_guard<std::mutex> audio(engineMutex);
              if(!engine||selected[ch].load()!=code||engine->sfzActive[ch]) continue;
              const auto* p=findPreset(engine->map,code);
              if(!p||p->sfzIndex<0) continue;
              index=p->sfzIndex; path=p->path; rate=engine->rate; serial=engine->serial;
              for(auto& slot:engine->sfzSources[static_cast<size_t>(index)].pool) if(slot&&slot->owner<0) {sfzRetry[ch].store(code);index=-1;break;}
            }
            if(index<0) continue;
            auto slot=createSfz(path,rate);
            if(!slot) {
                std::lock_guard<std::mutex> ui(viewMutex);
                if(!view.loading) {view.status="Unable to prepare SFZ for channel "+juce::String(ch+1)+": "+path; ++view.revision;}
                continue;
            }
            { std::lock_guard<std::mutex> audio(engineMutex);
              if(!engine||engine->serial!=serial||selected[ch].load()!=code||engine->sfzActive[ch]) continue;
              for(auto& available:engine->sfzSources[static_cast<size_t>(index)].pool) if(!available) {available=std::move(slot);break;}
              sfzRetry[ch].store(code);
            }
        }
    }
    void stateForSave(juce::String& path, Programs& p, Controls* controls=nullptr,MapEdits* edits=nullptr,int* sfzCode=nullptr,InstrumentGains* savedGains=nullptr,lsfx::NoteAssignments* savedNotes=nullptr) {
        std::lock_guard<std::mutex> lock(requestMutex);
        path=latest.path; if(edits) *edits=latest.edits; if(sfzCode) *sfzCode=latest.sfzCode;
        if(savedGains) {
            std::lock_guard<std::mutex> audio(engineMutex);
            *savedGains=latest.restoredGains?*latest.restoredGains:instrumentGains;
            if(latest.resetGainBank>=0) lsfx::eraseInstrumentGains(*savedGains,latest.resetGainBank,latest.resetGainProgram);
        }
        if(savedNotes) {
            std::lock_guard<std::mutex> notes(noteMutex);
            *savedNotes=latest.restoredNotes?*latest.restoredNotes:noteAssignments;
            if(latest.resetGainBank>=0) lsfx::clearAddress(*savedNotes,latest.resetGainBank,latest.resetGainProgram);
        }
        if(controls) *controls=(pending||loadingSerial!=0||failedSerial==latest.serial)?latest.controls:overrides;
        // While a new bank is loading retain the requested state, not old programs.
        if (pending || loadingSerial!=0 || failedSerial==latest.serial) p=latest.programs;
        else if(midiResetRequested.load()) p=initialPrograms(juce::File(latest.path).hasFileExtension("sfz"),latest.sfzCode);
        else for(int i=0;i<channels;++i) { int cmd=commands[i].load(); p[i]=cmd>=0 ? cmd : selected[i].load(); }
    }
    uint64_t loadingSerial=0, failedSerial=~uint64_t(0);
    void load(const juce::String& path,Programs p,Controls c=emptyControls(),MapEdits edits={},int sfzCode=0,bool preserveOnFailure=false,std::optional<InstrumentGains> restoredGains=std::nullopt,int resetGainBank=-1,int resetGainProgram=-1,std::optional<lsfx::NoteAssignments> restoredNotes=std::nullopt) {
        { std::lock_guard<std::mutex> lock(requestMutex);
          midiResetRequested.store(false);
          importRollback.reset();
          if(preserveOnFailure&&!pending&&loadingSerial==0) {
              importRollback=latest; rollbackWasFailed=failedSerial==latest.serial;
              if(!rollbackWasFailed) {
                  importRollback->controls=overrides;
                  for(int i=0;i<channels;++i) importRollback->programs[i]=current(i);
              }
          }
          latest={path,hostRate.load(),p,latest.serial+1,c,std::move(edits),sfzCode,std::move(restoredGains),resetGainBank,resetGainProgram,std::move(restoredNotes)}; pending=true;
          const bool empty=path.isEmpty()&&latest.edits.empty();
          if(empty) clearing.store(true);
          const auto display=mapDisplayPath(path,latest.edits);
          std::lock_guard<std::mutex> ui(viewMutex); view.loading=true; view.status=empty?"Unloading SoundFont":"Loading "+juce::File(display).getFileName(); ++view.revision;
        }
        cv.notify_one();
    }
    void work() {
        for(;;) {
            Request r;
            { std::unique_lock<std::mutex> lock(requestMutex); cv.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping||pending||extraSfzPending.load();}); if(stopping) return; if(!pending) {if(!extraSfzPending.exchange(false)) continue;lock.unlock();loadExtraSfz();continue;} r=latest; pending=false; loadingSerial=r.serial; }
            std::unique_ptr<Engine> fresh;
            std::vector<Preset> presets;
            std::map<juce::String,std::vector<CCLabel>> labels;
            juce::String error;
            const bool empty=r.path.isEmpty()&&r.edits.empty();
            if(!empty) {
                if(r.path.isNotEmpty()&&!juce::File(r.path).existsAsFile()) error="SoundFont not found: "+r.path;
                else {
                    fresh=std::make_unique<Engine>(); fresh->rate=r.rate; fresh->serial=r.serial; fresh->sfzCC=r.controls; fresh->resetPrograms=initialPrograms(juce::File(r.path).hasFileExtension("sfz"),r.sfzCode); fresh->prepareFX(); fresh->prepareMixer();
                    fresh->settings=new_fluid_settings();
                    if(fresh->settings) {
                        fluid_settings_setnum(fresh->settings,"synth.sample-rate",r.rate);
                        // Unity native gain, as in Scoring Synth. No artificial per-voice boost.
                        fluid_settings_setnum(fresh->settings,"synth.gain",1.0);
                        fluid_settings_setint(fresh->settings,"synth.midi-channels",16);
                        fluid_settings_setint(fresh->settings,"synth.audio-channels",sf2MultiOutput?sf2Groups:1);
                        fluid_settings_setint(fresh->settings,"synth.audio-groups",sf2MultiOutput?sf2Groups:1);
                        fluid_settings_setint(fresh->settings,"synth.effects-groups",1);
                        fluid_settings_setint(fresh->settings,"synth.polyphony",256);
                        fluid_settings_setint(fresh->settings,"synth.reverb.active",0);
                        fluid_settings_setint(fresh->settings,"synth.chorus.active",0);
                        fluid_settings_setstr(fresh->settings,"synth.midi-bank-select","gs");
                        fluid_settings_setint(fresh->settings,"synth.threadsafe-api",0);
                        fluid_settings_setint(fresh->settings,"synth.cpu-cores",1);
                        fresh->synth=new_fluid_synth(fresh->settings);
                    }
                    if(!fresh->synth) error="Unable to create FluidSynth";
                    else if(sf2MultiOutput&&(fluid_synth_count_audio_channels(fresh->synth)!=sf2Groups||fluid_synth_count_audio_groups(fresh->synth)!=sf2Groups||fluid_synth_count_effects_groups(fresh->synth)!=1||fluid_synth_count_effects_channels(fresh->synth)!=2)) {
                        error="Unsupported FluidSynth multigroup layout";
                    } else {
                        // Every distinct source is loaded once into the validated stereo synth.
                        std::map<juce::String,std::vector<Preset>> sources;
                        auto source=[&](const juce::String& path)->const std::vector<Preset>* {
                            if(auto it=sources.find(path);it!=sources.end()) return &it->second;
                            if(!juce::File(path).existsAsFile()) {error="SoundFont not found: "+path;return nullptr;}
                            const int font=fluid_synth_sfload(fresh->synth,path.toRawUTF8(),fresh->font<0?1:0);
                            if(font<0) {error="Unable to load SoundFont: "+path;return nullptr;}
                            if(fresh->font<0) fresh->font=font;
                            return &sources.emplace(path,catalogSF2(fresh->synth,font,path)).first->second;
                        };
                        std::map<int,Preset> mapped;
                        auto mapSfz=[&](const juce::String& path,int bank,int program) {
                            if(!juce::File(path).existsAsFile()) {error="SoundFont not found: "+path;return;}
                            mapped[pack(bank,program)]={bank,program,juce::File(path).getFileNameWithoutExtension(),path,0,0,-1,-1};
                        };
                        if(r.path.isNotEmpty()) {
                            if(juce::File(r.path).hasFileExtension("sfz")) mapSfz(r.path,r.sfzCode>>7,r.sfzCode&127);
                            else if(auto* list=source(r.path)) for(const auto& p:*list) mapped[pack(p.bank,p.program)]=p;
                        }
                        for(const auto& e:r.edits) {
                            if(error.isNotEmpty()) break;
                            if(e.clear) {lsfx::clearAddress(mapped,e.bank,e.program);continue;}
                            if(juce::File(e.path).hasFileExtension("sfz")) { if(e.program<0) {error="SFZ cannot replace a bank";break;} mapSfz(e.path,e.bank,e.program); continue; }
                            const auto* list=source(e.path); if(!list) break;
                            if(e.program<0) {
                                if(!lsfx::replaceBank(mapped,*list,e.bank,e.sourceBank)) error="Source bank no longer exists: "+e.path;
                            } else if(auto* p=findPreset(*list,pack(e.sourceBank,e.sourceProgram))) {
                                lsfx::assignProgram(mapped,*p,e.bank,e.program);
                            } else error="Source preset no longer exists: "+e.path;
                        }
                        if(error.isEmpty()) {
                            std::map<juce::String,int> sfzIndices;
                            for(auto& item:mapped) {
                                auto& p=item.second;
                                if(juce::File(p.path).hasFileExtension("sfz")) {
                                    auto it=sfzIndices.find(p.path);
                                    if(it==sfzIndices.end()) {
                                        const int index=static_cast<int>(fresh->sfzSources.size());
                                        SfzSource entry; entry.path=p.path;
                                        // One preload per distinct source; extra copies only for currently selected channels.
                                        int copies=0;
                                        for(int code:r.programs) {auto pos=mapped.find(code); if(pos!=mapped.end()&&pos->second.path==p.path) ++copies;}
                                        copies=juce::jmax(1,copies);
                                        for(int i=0;i<copies;++i) {
                                            entry.pool[i]=createSfz(p.path,r.rate,i==0?&entry.labels:nullptr,i==0?&entry.initialCC:nullptr);
                                            if(!entry.pool[i]) {error="Unable to load SFZ: "+p.path;break;}
                                        }
                                        if(error.isNotEmpty()) break;
                                        labels.emplace(p.path,entry.labels);
                                        fresh->sfzSources.push_back(std::move(entry));
                                        it=sfzIndices.emplace(p.path,index).first;
                                    }
                                    p.sfzIndex=it->second;
                                } else fresh->hasSF2=true;
                            }
                            if(error.isEmpty()) {
                                for(auto& item:mapped) presets.push_back(std::move(item.second));
                                fresh->map=presets;
                                fluid_synth_set_interp_method(fresh->synth,-1,FLUID_INTERP_4THORDER);
                                for(int i=0;i<channels;++i) if(!choose(*fresh,i,r.programs[i])) {error="Unable to allocate SFZ channel";break;}
                            }
                        }
                    }
                }
            }
            std::unique_ptr<Engine> retired;
            { std::lock_guard<std::mutex> lock(requestMutex);
              if(stopping) return;
              if(r.serial!=latest.serial) { loadingSerial=0; continue; }
              if(error.isNotEmpty()&&importRollback) {
                  latest=std::move(*importRollback); latest.serial=r.serial; importRollback.reset();
                  loadingSerial=0; failedSerial=rollbackWasFailed?r.serial:~uint64_t(0);
                  std::lock_guard<std::mutex> ui(viewMutex);
                  view.loading=false; view.status="Import failed: "+error+"; previous map retained"; ++view.revision;
                  continue;
              }
              importRollback.reset();
              // Failed full replacement unloads the previous bank deliberately: no wrong-bank playback.
              { std::lock_guard<std::mutex> audio(engineMutex);
                if(r.restoredGains) instrumentGains=std::move(*r.restoredGains);
                if(error.isEmpty()&&r.resetGainBank>=0) lsfx::eraseInstrumentGains(instrumentGains,r.resetGainBank,r.resetGainProgram);
                retired=std::move(engine);
                if(error.isEmpty()) engine=std::move(fresh);
                overrides=r.controls; controlDirty.store(false);
                for(int i=0;i<controlCount;++i) { controlValues[i].store(unset); controlCommands[i].store(unset); }
                if(engine) {
                    for(int ch=0;ch<channels;++ch) {
                        engine->channelGains[ch].setCurrentAndTargetValue(juce::Decibels::decibelsToGain(channelGain[ch]->get(),channelGainMin-1.0f));
                        const auto* preset=findPreset(engine->map,r.programs[ch]);
                        if(preset&&preset->sfzIndex>=0) readSfzControls(*engine,ch,*preset);
                        else refreshFluidControls(engine->synth,ch);
                    }
                    // Restore explicit overrides on the first audio callback.
                    for(int i=0;i<controlCount;++i) if(r.controls[i]!=unset) {
                        controlCommands[i].store(r.controls[i]); controlDirty.store(true);
                    }
                }
                for(int i=0;i<channels;++i) { commands[i].store(-1); sfzNeeded[i].store(-1); sfzRetry[i].store(-1); selected[i].store(r.programs[i]); }
                if(empty) {
                    activeVoices.store(0); sleeping.store(false); droppedBlocks.store(0); noteRouteOverflow.store(0);
                    programChanges.store(0); lastProgram.store(-1); reverbCC.store(0);
                    chorusCC.store(0); sysexCount.store(0); fxSysexCount.store(0);
                    panic.store(false);
                }
                clearing.store(false);
              { std::lock_guard<std::mutex> notes(noteMutex);
                if(r.restoredNotes) noteAssignments=std::move(*r.restoredNotes);
                if(error.isEmpty()&&r.resetGainBank>=0) lsfx::clearAddress(noteAssignments,r.resetGainBank,r.resetGainProgram);
              }
              }
              latest.restoredNotes.reset(); latest.restoredGains.reset(); latest.resetGainBank=-1; latest.resetGainProgram=-1;
              loadingSerial=0; failedSerial=error.isNotEmpty()?r.serial:~uint64_t(0);
              { std::lock_guard<std::mutex> ui(viewMutex);
                view.ready=error.isEmpty()&&!empty; view.labels=std::move(labels); view.path=mapDisplayPath(r.path,r.edits); view.presets=error.isEmpty()?std::move(presets):std::vector<Preset>{}; view.loading=false;
                view.status=error.isNotEmpty()?error:empty?"No SoundFont loaded":"Loaded: "+juce::File(view.path).getFileName()+"; "+juce::String(static_cast<int>(view.presets.size()))+" mapped programs"+(sf2MultiOutput?juce::String():juce::String("; SF2 Main 1/2 only"));
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
constexpr int rowGain=-1,rowInterpolation=-2,rowEffects=-3,rowSfzQuality=-4,rowChannel=-5,rowMidiFX=-6,rowRoute=-7,rowInstrumentGain=-8;
constexpr int compRowBase=-200;
int compRow(int i) {return compRowBase-i;}
int compIndex(int row) {const int i=compRowBase-row;return i>=0&&i<lsfx::compCount?i:-1;}
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

class GridNavigator : public juce::TextButton {
public:
    GridNavigator() : juce::TextButton("Parameters") {}
    int getSelectedItemIndex() const { return selected; }
    int getSelectedId() const { return selected+1; }
    void clear(juce::NotificationType) { items.clear(); selected=-1; }
    void addItem(const juce::String& text,int) { items.add(text); }
    juce::String getItemText(int i) const { return items[i]; }
    void changeItemText(int id,const juce::String& text) { items.set(id-1,text); if(selected==id-1) updateLabel(); }
    void setSelectedId(int id,juce::NotificationType n) { setSelectedItemIndex(id-1,n); }
    void setSelectedItemIndex(int i,juce::NotificationType) { selected=juce::isPositiveAndBelow(i,items.size())?i:-1; updateLabel(); }
private:
    juce::StringArray items;
    int selected=-1;
    void updateLabel() { auto label=selected>=0?items[selected]:juce::String("Parameters"); if(getButtonText()!=label) {setButtonText(label);setName(label);setTitle(label);} }
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
#if !JUCE_WINDOWS
    source.setDescription(message);
    if(auto* handler=source.getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
#endif
    // No generic JUCE announcement fallback: it may use standalone speech.
}

#include "InstrumentEditorPanel.h"

class Editor final : public juce::AudioProcessorEditor, private juce::Timer, private juce::KeyListener, private juce::ListBoxModel, private juce::FocusChangeListener {
public:
    explicit Editor(Processor& p) : AudioProcessorEditor(p), proc(p) {
        setWantsKeyboardFocus(true);
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        title.setText(sf2MultiOutput?"LSFX 0.99.3 TEST37":"LSFX 0.99.3 TEST37 SF2 stereo recovery",juce::dontSendNotification); title.setFont(juce::FontOptions(21.0f)); title.setAccessible(false); addAndMakeVisible(title);
        status.setText("No SoundFont loaded",juce::dontSendNotification); status.setTitle("Instrument status"); addAndMakeVisible(status);
        setup(open,"Open SF2 or SFZ",1); setup(parameters,"Parameters",2); setup(channel,"MIDI channel",3); setup(bank,"Bank",37); setup(program,"Program",38); setup(level,"Master output gain",36);
        setup(grid,"Parameter grid",1); setup(value,"Parameter value",2); setup(closeGrid,"Close parameters",3);
        open.setDescription("Alt+O"); parameters.setDescription("Alt+L"); channel.setDescription("Alt+M"); bank.setDescription("Alt+K"); program.setDescription("Alt+P"); level.setDescription("Alt+G");
        grid.setDescription({}); value.setDescription({});
        channelLabel.setText("MIDI channel",juce::dontSendNotification); bankLabel.setText("Bank",juce::dontSendNotification); programLabel.setText("Program",juce::dontSendNotification); levelLabel.setText("Master output gain (dB)",juce::dontSendNotification);
        gridTitle.setText("Parameters",juce::dontSendNotification); hint.setText("Arrows: grid or value. Type to search. Enter: edit / browse. Esc: close.",juce::dontSendNotification);
        for(auto* l:{&channelLabel,&bankLabel,&programLabel,&levelLabel,&gridTitle,&hint}) addAndMakeVisible(*l);
        for(int i=1;i<=16;++i) channel.addItem("Channel "+juce::String(i),i);
        channel.setSelectedId(proc.uiChannel()+1,juce::dontSendNotification);
        channel.onChange=[this]{ if(cached.loading||catalogBusy||importInProgress) {refresh(true);return;} proc.setUiChannel(channel.getSelectedId()-1); shownCode=-1; refresh(true); if(gridOpen) rebuildGrid(); };
        bank.onChange=[this]{ if(!updating&&(cached.loading||catalogBusy||importInProgress)) {refresh(true);return;} if(!updating) { rebuildPrograms(bank.getSelectedId()-1,-1); if(program.getNumItems()>0) { program.setSelectedItemIndex(0,juce::dontSendNotification); apply(); } } };
        program.onChange=[this]{ if(!updating&&(cached.loading||catalogBusy||importInProgress)) {refresh(true);return;} if(!updating) apply(); };
        open.onClick=[this]{ browse(); }; parameters.onClick=[this]{ openParameters(); };
        closeGrid.onClick=[this]{ closeParameters(); };
        level.setSliderStyle(juce::Slider::LinearHorizontal); level.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0); level.textFromValueFunction=[](double v){return juce::String(v,1)+" dB";}; level.setRange(-60.0,6.0,0.1); level.setValue(proc.gainParameter().get(),juce::dontSendNotification);
        level.onDragStart=[this]{proc.gainParameter().beginChangeGesture(); dragging=true;};
        level.onDragEnd=[this]{dragging=false;proc.gainParameter().endChangeGesture();};
        level.onValueChange=[this]{ auto& a=proc.gainParameter(); if(!dragging) a.beginChangeGesture(); a.setValueNotifyingHost(a.convertTo0to1(static_cast<float>(level.getValue()))); if(!dragging) a.endChangeGesture(); levelReadout.setText(juce::String(level.getValue(),1)+" dB",juce::dontSendNotification); };
        channelGainLabel.setText("Channel gains / SF2 voice trims (dB) - Alt+U",juce::dontSendNotification);
        channelGainLabel.setAccessible(false); addAndMakeVisible(channelGainLabel);
        for(int ch=0;ch<channels;++ch) {
            auto& c=channelLevels[ch];
            setup(c,"Output gain channel "+juce::String(ch+1),4+ch);
            c.setDescription({});
            c.setSliderStyle(juce::Slider::LinearVertical); c.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0);
            c.setRange(channelGainMin,channelGainMax,0.1); c.textFromValueFunction=[](double v){return juce::String(v,1)+" dB";};
            c.setValue(proc.channelGainParameter(ch).get(),juce::dontSendNotification);
            c.onDragStart=[this,ch]{proc.channelGainParameter(ch).beginChangeGesture();channelDragging[ch]=true;lastGainChannel=ch;};
            c.onDragEnd=[this,ch]{channelDragging[ch]=false;proc.channelGainParameter(ch).endChangeGesture();};
            c.onValueChange=[this,ch]{auto& a=proc.channelGainParameter(ch);if(!channelDragging[ch]) a.beginChangeGesture(); a.setValueNotifyingHost(a.convertTo0to1(float(channelLevels[ch].getValue())));if(!channelDragging[ch]) a.endChangeGesture(); repaint();};
        }
        routingLabel.setText("Stereo output routing - Alt+R",juce::dontSendNotification);
        routingLabel.setAccessible(false); addAndMakeVisible(routingLabel);
        for(int ch=0;ch<channels;++ch) {
            auto& c=routeLevels[ch];
            setup(c,"Output routing channel "+juce::String(ch+1),20+ch);
            c.setDescription({});
            c.setSliderStyle(juce::Slider::LinearVertical); c.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0);
            c.setRange(0,stereoBuses-1,1); c.textFromValueFunction=[this,ch](double v){return routingValue(v,ch);};
            c.setValue(proc.routeParameter(ch).getIndex(),juce::dontSendNotification);
            c.onDragStart=[this,ch]{proc.routeParameter(ch).beginChangeGesture();routeDragging[ch]=true;lastRouteChannel=ch;};
            c.onDragEnd=[this,ch]{routeDragging[ch]=false;proc.routeParameter(ch).endChangeGesture();};
            c.onValueChange=[this,ch]{auto& a=proc.routeParameter(ch);if(!routeDragging[ch]) a.beginChangeGesture();a.setValueNotifyingHost(a.convertTo0to1(float(routeLevels[ch].getValue())));if(!routeDragging[ch]) a.endChangeGesture();repaint();};
        }
        value.setSliderStyle(juce::Slider::LinearHorizontal); value.setTextBoxStyle(juce::Slider::NoTextBox,true,0,0);
        for(auto* l:{&levelReadout,&valueReadout}) {l->setJustificationType(juce::Justification::centredRight); l->setAccessible(false); addAndMakeVisible(*l);}
        grid.onClick=[this]{ if(gridOpen) focus(value); };
        value.onValueChange=[this]{ if(!updatingGrid) { if(auto* row=selectedRow()) { const auto r=*row; writeRow(r,value.getValue()); refreshGridValues(); } } };
        // Keyed controls intercept their own arrows before JUCE's native behavior;
        // the editor listener also handles Alt accelerators bubbling from children.
        addKeyListener(this);
        setup(presetsButton,"Preset browser",39); presetsButton.setDescription("Alt+B"); presetsButton.onClick=[this]{openPresetBrowser();};
        setup(saveButton,"Save preset",40); saveButton.setDescription("Ctrl+S"); saveButton.onClick=[this]{saveCurrent();};
        setup(saveAsButton,"Save preset as",41); saveAsButton.setDescription("Ctrl+Shift+S"); saveAsButton.onClick=[this]{showSaveAs();};
        presetList.setName("Preset browser"); presetList.setTitle("Preset browser"); presetList.setWantsKeyboardFocus(true); presetList.setExplicitFocusOrder(1); presetList.addKeyListener(this); addAndMakeVisible(presetList); presetList.setModel(this); presetList.setRowHeight(30); presetList.setMultipleSelectionEnabled(false);
        setup(parentButton,"Back",2); parentButton.onClick=[this]{parentFolder();};
        setup(panelOK,"OK",2); panelOK.onClick=[this]{if(panelAction) { auto action=panelAction; action(); }};
        setup(panelCancel,"Cancel",3); panelCancel.onClick=[this]{cancelPanel();};
        setup(noteCell,"Note assignments",1);noteCell.onClick=[this]{updateNoteCell(true);};
        panelText.setTitle("Preset name"); panelText.setWantsKeyboardFocus(true); panelText.setExplicitFocusOrder(1); panelText.addKeyListener(this); addAndMakeVisible(panelText);
        panelText.onReturnKey=[this]{if(panelMode==Panel::name&&panelAction) {auto action=panelAction;action();}};
        panelText.onEscapeKey=[this]{cancelPanel();};
        panelLabel.setTitle("Preset information"); panelLabel.setJustificationType(juce::Justification::centredLeft); addAndMakeVisible(panelLabel);
        setup(exportButton,"Export SoundFont",42); exportButton.setDescription("Alt+E"); exportButton.onClick=[this]{browseExport();};
        setup(instrumentButton,"Instrument Editor",43); instrumentButton.setDescription("Alt+T"); instrumentButton.onClick=[this]{openInstrumentEditor();};
        setup(aboutButton,"About",44); aboutButton.setDescription("Alt+A"); aboutButton.onClick=[this]{openAbout();};
        const auto aboutText=juce::String("LSFX\nVersion: ")+lsfxVersion+"\nRelease date: "+lsfxReleaseDate+"\nLicense: "+lsfxLicense+"\nProject: "+lsfxProjectUrl+"\nContact: "+lsfxContactEmail;
        setup(aboutInfo,"About LSFX",1);
        aboutInfo.setMultiLine(true,true); aboutInfo.setReturnKeyStartsNewLine(false); aboutInfo.setReadOnly(true);
        aboutInfo.setScrollbarsShown(true); aboutInfo.setCaretVisible(true); aboutInfo.setPopupMenuEnabled(false);
        aboutInfo.setTabKeyUsedAsCharacter(false); aboutInfo.setText(aboutText,false); aboutInfo.setDescription(aboutText);
        aboutInfo.setJustification(juce::Justification::centredLeft); aboutInfo.setFont(juce::Font(juce::FontOptions(18.0f)));
        aboutInfo.setBorder(juce::BorderSize<int>(12)); aboutInfo.onEscapeKey=[this]{closePanel();};
        setup(aboutProject,juce::String("Project: ")+lsfxProjectUrl,2);
        aboutProject.setButtonText(juce::String("Project: ")+lsfxProjectUrl);
        aboutProject.setDescription("LSFX GitHub project URL. Press Enter to open in the default browser.");
        aboutProject.onClick=[this]{openAboutLink(false);};
        setup(aboutContact,juce::String("Contact: ")+lsfxContactEmail,3);
        aboutContact.setButtonText(juce::String("Contact: ")+lsfxContactEmail);
        aboutContact.setDescription("LSFX contact email. Press Enter to open the default email application.");
        aboutContact.onClick=[this]{openAboutLink(true);};
        setup(aboutClose,"Close",4); aboutClose.setDescription("Closes About. Shortcut Alt C"); aboutClose.onClick=[this]{closePanel();};
        hidePanelControls();
        presetRoot().createDirectory();
        lastGainChannel=proc.editorMemory.gainChannel; lastRouteChannel=proc.editorMemory.routeChannel;
        lastSf2Row=proc.editorMemory.sf2Row; lastSfzRow=proc.editorMemory.sfzRow; stepIndex=proc.editorMemory.step;
        showGrid(false); setSize(900,690); refresh(true);
        if(isGridFocus(proc.editorMemory.focus)) {rebuildGrid();showGrid(true);}
        juce::Desktop::getInstance().addFocusChangeListener(this);
        startTimerHz(5);
        scheduleInitialFocus();
    }
    ~Editor() override { if(instrumentPanel)proc.stopInstrumentPreview(); proc.setNoteLearnChannel(-1); if(panelMode==Panel::none&&!initialFocusPending&&!returnFromChooser&&!chooser&&!importInProgress) rememberFocus(focusId(juce::Component::getCurrentlyFocusedComponent())); juce::Desktop::getInstance().removeFocusChangeListener(this); stopTimer(); removeKeyListener(this); panelText.removeKeyListener(this); presetList.removeKeyListener(this); presetList.setModel(nullptr); chooser.reset(); if(catalogThread.joinable()) catalogThread.join(); if(exportJob) exportJob->cancel.store(true); if(exportThread.joinable()) exportThread.join(); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff202429));
        if(panelMode==Panel::notes) {paintNoteGrid(g);return;}
        if(panelMode!=Panel::none) return;
        if(!gridOpen) {
            g.setFont(juce::Font(juce::FontOptions(11.0f))); g.setColour(juce::Colours::white);
            for(int ch=0;ch<channels;++ch) {
                const auto cell=channelLevels[ch].getBounds();
                g.drawText(juce::String(ch+1),cell.withY(cell.getY()-15).withHeight(15),juce::Justification::centred,false);
                g.drawText(juce::String(channelLevels[ch].getValue(),1),cell.withY(cell.getBottom()).withHeight(15),juce::Justification::centred,false);
                const auto routing=routeLevels[ch].getBounds();
                g.drawText(juce::String(ch+1),routing.withY(routing.getY()-15).withHeight(15),juce::Justification::centred,false);
                g.drawText(routeName(juce::roundToInt(routeLevels[ch].getValue())),routing.withY(routing.getBottom()).withHeight(15),juce::Justification::centred,false);
            }
            return;
        }
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
            g.setColour(juce::Colours::white); g.drawFittedText(rowName(row),content,juce::Justification::centredLeft,1);
            g.setColour(juce::Colour(0xffd0e4f7)); g.drawFittedText(formatValue(row,readRow(row)),right,juce::Justification::centredRight,1);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if(panelMode==Panel::notes) {
            if(noteArea.contains(e.getPosition())) {
                const auto list=visibleNoteRows();const auto first=notePageStart(list);
                const int row=(e.getPosition().y-noteArea.getY())/25-1;
                const int col=(e.getPosition().x-noteArea.getX())/juce::jmax(1,noteArea.getWidth()/4);
                if(row>=0&&row<12&&first+row<int(list.size())) {noteSelected=list[size_t(first+row)];noteColumn=juce::jlimit(0,3,col);focus(noteCell);updateNoteCell(true);}
            }
            return;
        }
        if(panelMode!=Panel::none||!gridOpen||!gridArea.contains(e.getPosition())) return;
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
        if(panelMode==Panel::instrument) {if(instrumentPanel) instrumentPanel->setBounds(r);return;}
        if(panelMode==Panel::about) {
            auto a=r.withSizeKeepingCentre(juce::jmin(700,r.getWidth()),juce::jmin(430,r.getHeight()));
            auto close=a.removeFromBottom(42); a.removeFromBottom(10);
            auto contact=a.removeFromBottom(38); a.removeFromBottom(8);
            auto project=a.removeFromBottom(38); a.removeFromBottom(10);
            aboutInfo.setBounds(a); aboutProject.setBounds(project); aboutContact.setBounds(contact);
            aboutClose.setBounds(close.withSizeKeepingCentre(140,36)); return;
        }
        if(panelMode==Panel::notes) {
            panelLabel.setBounds(r.removeFromTop(88));r.removeFromTop(5);
            noteCell.setBounds(r.removeFromTop(38));r.removeFromTop(10);
            panelCancel.setBounds(r.removeFromBottom(38).removeFromRight(150));
            noteArea=r.removeFromTop(325);return;
        }
        if(panelMode!=Panel::none) {
            panelLabel.setBounds(r.removeFromTop(82)); r.removeFromTop(8);
            if(panelMode==Panel::browser||panelMode==Panel::source) { auto buttons=r.removeFromBottom(38); parentButton.setBounds(buttons.removeFromLeft(150)); panelCancel.setBounds(buttons.removeFromRight(150)); presetList.setBounds(r); }
            else { if(panelMode==Panel::name) panelText.setBounds(r.removeFromTop(36)); r.removeFromTop(12); auto buttons=r.removeFromTop(38); panelOK.setBounds(buttons.removeFromLeft(180)); panelCancel.setBounds(buttons.removeFromLeft(180)); }
            return;
        }
        if(gridOpen) {
            r.removeFromTop(8); gridTitle.setBounds(r.removeFromTop(28)); grid.setBounds(r.removeFromTop(34)); r.removeFromTop(6);
            auto v=r.removeFromTop(38); valueReadout.setBounds(v.removeFromRight(180)); value.setBounds(v.reduced(0,2));
            r.removeFromTop(6); gridPageBounds=r.removeFromTop(20); gridArea=r.removeFromTop(240);
            r.removeFromTop(6); hint.setBounds(r.removeFromTop(30)); closeGrid.setBounds(r.removeFromTop(34)); return;
        }
        open.setBounds(r.removeFromTop(30)); r.removeFromTop(4); parameters.setBounds(r.removeFromTop(30)); r.removeFromTop(8);
        auto line=[&](juce::Label& l,juce::Component& c){auto a=r.removeFromTop(38);l.setBounds(a.removeFromLeft(150));c.setBounds(a.reduced(0,3));};
        line(channelLabel,channel);
        channelGainLabel.setBounds(r.removeFromTop(24));
        auto strip=r.removeFromTop(104).reduced(0,15);
        const int cellWidth=strip.getWidth()/channels;
        for(int ch=0;ch<channels;++ch) channelLevels[ch].setBounds(strip.getX()+ch*cellWidth,strip.getY(),cellWidth,strip.getHeight());
        routingLabel.setBounds(r.removeFromTop(24));
        auto routes=r.removeFromTop(104).reduced(0,15);
        for(int ch=0;ch<channels;++ch) routeLevels[ch].setBounds(routes.getX()+ch*cellWidth,routes.getY(),cellWidth,routes.getHeight());
        auto lv=r.removeFromTop(38); levelLabel.setBounds(lv.removeFromLeft(200)); levelReadout.setBounds(lv.removeFromRight(100)); level.setBounds(lv.reduced(0,3));
        line(bankLabel,bank);line(programLabel,program);
        auto buttons=r.removeFromTop(34); presetsButton.setBounds(buttons.removeFromLeft(140)); saveButton.setBounds(buttons.removeFromLeft(125)); saveAsButton.setBounds(buttons.removeFromLeft(140)); exportButton.setBounds(buttons.removeFromLeft(160)); instrumentButton.setBounds(buttons.removeFromLeft(180)); aboutButton.setBounds(buttons.removeFromLeft(100));
    }
private:
    Processor& proc;
    juce::Label title,status,channelLabel,bankLabel,programLabel,levelLabel,gridTitle,hint,levelReadout,valueReadout;
    juce::Rectangle<int> gridArea,gridPageBounds;
    ljuno::IncrementalParameterSearch search;
    Keyed<juce::TextButton> open{"Open SF2 or SFZ"},parameters{"Parameters"},closeGrid{"Close parameters"};
    Keyed<juce::ComboBox> channel,bank,program;
    Keyed<GridNavigator> grid;
    Keyed<juce::TextButton> presetsButton{"Preset browser"},saveButton{"Save preset"},saveAsButton{"Save preset as"};
    juce::ListBox presetList{"Preset browser",nullptr};
    Keyed<juce::TextButton> parentButton{"Back"},panelOK{"OK"},panelCancel{"Cancel"};
    juce::TextEditor panelText;
    juce::Label panelLabel;
    enum class Panel {none,browser,name,confirm,source,about,exportProgress,notes,instrument};
    Keyed<juce::TextButton> noteCell{"Note assignments"};
    int noteProgram=0,noteChannel=0,noteSelected=-1,noteColumn=0;
    bool noteLearn=false;
    uint64_t noteEventSeen=0;
    std::map<int,lsfx::NoteAssignment> noteRows;
    juce::Rectangle<int> noteArea;
    Keyed<juce::TextButton> exportButton{"Export SoundFont"},instrumentButton{"Instrument Editor"};
    std::unique_ptr<InstrumentEditorPanel> instrumentPanel;
    struct ExportJob {std::atomic<bool> cancel{false},done{false}; std::atomic<int> progress{0}; juce::String message;};
    std::shared_ptr<ExportJob> exportJob;
    std::thread exportThread;
    int exportProgressShown=-1;
    Keyed<juce::TextButton> aboutButton{"About"},aboutProject{"Project"},aboutContact{"Contact"},aboutClose{"Close"};
    Keyed<juce::TextEditor> aboutInfo{"About LSFX"};
    Panel panelMode=Panel::none;
    juce::File browserDirectory;
    juce::String lastRememberedInstrument;
    juce::File pendingBrowserPreset;
    std::vector<juce::File> browserEntries;
    std::function<void()> panelAction,panelCancelAction;
    std::vector<std::pair<int,juce::String>> sourceEntries;
    std::function<void(int)> sourceAction;
    int panelReturn=1,chooserReturn=1;
    bool importInProgress=false,focusTransition=false,hadEditorFocus=false;
    uint64_t missingPromptRevision=~uint64_t(0);
    Keyed<juce::Slider> level,value;
    std::array<Keyed<juce::Slider>,channels> channelLevels;
    std::array<bool,channels> channelDragging{};
    juce::Label channelGainLabel,routingLabel;
    std::array<Keyed<juce::Slider>,channels> routeLevels;
    std::array<bool,channels> routeDragging{};
    int lastRouteChannel=0;
    int lastGainChannel=0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::thread catalogThread;
    bool catalogBusy=false;
    View cached;
    std::vector<GridRow> rows;
    uint64_t revision=~uint64_t(0);
    int shownCode=-1,gridShownCode=-1,stepIndex=0,lastSf2Row=rowGain,lastSfzRow=rowGain;
    bool updating=false,dragging=false,gridOpen=false,updatingGrid=false,initialFocusPending=true,focusScheduled=false;
    bool returnFromChooser=false,announceLoaded=false;
    juce::String pendingReturnMessage;
    uint32_t chooserReturnedAt=0;
    template<class C> void setup(Keyed<C>& c,const juce::String& name,int order) {
        c.setName(name); c.setTitle(name); c.setWantsKeyboardFocus(true); c.setExplicitFocusOrder(order);
        c.keyHandler=[this](const juce::KeyPress& k,juce::Component* origin){return handleKey(k,origin);}; addAndMakeVisible(c);
    }
    juce::Rectangle<int> gridCell(int k) const {
        const int col=k/gridRowsPerColumn, row=k%gridRowsPerColumn;
        const int w=gridArea.getWidth()/gridColumnsPerPage, h=gridArea.getHeight()/gridRowsPerColumn;
        return {gridArea.getX()+col*w,gridArea.getY()+row*h,w,h};
    }
    static bool isGridFocus(int id) {return id>=50&&id<=52;}
    int focusId(juce::Component* c) {
        for(;c&&c!=this;c=c->getParentComponent()) {
            if(c==&open)return 1; if(c==&parameters)return 2; if(c==&channel)return 3;
            if(c==&level)return 36; if(c==&bank)return 37; if(c==&program)return 38;
            if(c==&presetsButton)return 39; if(c==&saveButton)return 40; if(c==&saveAsButton)return 41; if(c==&aboutButton)return 42; if(c==&exportButton)return 43; if(c==&instrumentButton)return 44;
            if(c==&grid)return 50; if(c==&value)return 51; if(c==&closeGrid)return 52;
            for(int i=0;i<channels;++i) {if(c==&channelLevels[i])return 4+i; if(c==&routeLevels[i])return 20+i;}
        } return 0;
    }
    juce::Component& focusTarget(int id) {
        if(id>=4&&id<20)return channelLevels[id-4]; if(id>=20&&id<36)return routeLevels[id-20];
        switch(id) {case 2:return parameters;case 3:return channel;case 36:return level;case 37:return bank;case 38:return program;case 39:return presetsButton;case 40:return saveButton;case 41:return saveAsButton;case 42:return aboutButton;case 43:return exportButton;case 44:return instrumentButton;case 50:return grid;case 51:return value;case 52:return closeGrid;default:return open;}
    }
    void rememberFocus(int id) {
        if(!id)return;
        proc.editorMemory.focus=id;
        if(id>=4&&id<20) lastGainChannel=id-4;
        if(id>=20&&id<36) lastRouteChannel=id-20;
        proc.editorMemory.gainChannel=lastGainChannel; proc.editorMemory.routeChannel=lastRouteChannel;
    }
    void globalFocusChanged(juce::Component* c) override {
        const bool inside=c&&(c==this||isParentOf(c));
        if(!inside) {if(hadEditorFocus&&!chooser&&!importInProgress&&panelMode==Panel::none) initialFocusPending=true; hadEditorFocus=false;return;}
        hadEditorFocus=true;
        if(focusTransition||chooser||importInProgress||returnFromChooser||panelMode!=Panel::none)return;
        if(initialFocusPending) {scheduleInitialFocus();return;}
        rememberFocus(focusId(c));
    }
    void restoreFocus(int id) {
        juce::ScopedValueSetter<bool> guard(focusTransition,true);
        if(isGridFocus(id)&&!gridOpen) {rebuildGrid();showGrid(true);}
        else if(!isGridFocus(id)&&gridOpen) showGrid(false,&focusTarget(id));
        initialFocusPending=false; rememberFocus(id); focus(focusTarget(id));
    }
    void returnTo(int id) {
        chooserReturn=id; returnFromChooser=true; chooserReturnedAt=juce::Time::getMillisecondCounter();
        initialFocusPending=true; rememberFocus(id);
        // A Windows file dialog can still be releasing native focus in its
        // completion callback. Do not treat cached JUCE focus as success yet.
        scheduleInitialFocus();
    }
    bool peerReadyForReturn() {
        auto* peer=getPeer();
        if(!peer||!isShowing()) return false;
        if(peer->isFocused()) return true;
#if JUCE_WINDOWS
        if(returnFromChooser) {
            const auto hwnd=reinterpret_cast<HWND>(peer->getNativeHandle());
            const auto foreground=GetForegroundWindow();
            const auto nativeFocus=GetFocus();
            // Reclaim focus only in the same foreground window, if Windows
            // returned to the parent/owner (not a sibling REAPER control).
            if(hwnd&&foreground&&GetAncestor(hwnd,GA_ROOT)==GetAncestor(foreground,GA_ROOT)
                &&(!nativeFocus||nativeFocus==hwnd||IsChild(nativeFocus,hwnd))) {
                peer->grabFocus();
                return peer->isFocused();
            }
        }
#endif
        return false;
    }
    void publishReturnedFocus(juce::Component& target) {
        if(!target.hasKeyboardFocus(true)) return;
        // Recreate the accessibility focus event even if JUCE still believes
        // this control is focused after the native dialog closes.
        juce::AccessibilityHandler::clearCurrentlyFocusedHandler();
        if(auto* h=target.getAccessibilityHandler()) h->grabFocus();
        if(pendingReturnMessage.isNotEmpty()) speak(target,pendingReturnMessage);
        else if(&target==&bank) speak(bank,bank.getText());
        else if(&target==&program) speak(program,program.getText());
        pendingReturnMessage.clear();
    }
    // Do not publish a UIA focus event unless the native keyboard focus really arrived.
    // Never take focus back from REAPER's FX list or another child on a timer.
    void focus(juce::Component& c) {
        if(!c.isShowing()||!c.isEnabled()) return;
        if(!c.hasKeyboardFocus(true)) {
            juce::AccessibilityHandler::clearCurrentlyFocusedHandler(); c.grabKeyboardFocus();
            if(c.hasKeyboardFocus(true)) if(auto* handler=c.getAccessibilityHandler()) handler->grabFocus();
        }
        if(c.hasKeyboardFocus(true)&&panelMode==Panel::none&&!focusTransition&&!importInProgress&&!returnFromChooser) {initialFocusPending=false;rememberFocus(focusId(&c));}
    }
    void scheduleInitialFocus() {
        if(panelMode!=Panel::none||(!initialFocusPending&&!returnFromChooser)||focusScheduled||!isShowing()) return;
        focusScheduled=true;
        juce::Component::SafePointer<Editor> safe(this);
        juce::Timer::callAfterDelay(150,[safe]{if(auto* e=safe.getComponent()) {
            e->focusScheduled=false;
            if(e->panelMode!=Panel::none||(!e->initialFocusPending&&!e->returnFromChooser)||!e->isShowing()||e->chooser) return;
            if(e->returnFromChooser&&juce::Time::getMillisecondCounter()-e->chooserReturnedAt>5000u) e->returnFromChooser=false;
            if(e->importInProgress||!e->peerReadyForReturn()) return;
            auto* focused=juce::Component::getCurrentlyFocusedComponent();
            if(focused!=nullptr&&focused!=e&&!e->isParentOf(focused)) return;
            const bool returning=e->returnFromChooser;
            const int id=returning?e->chooserReturn:e->proc.editorMemory.focus;
            auto& target=e->focusTarget(id);
            e->restoreFocus(id);
            if(target.hasKeyboardFocus(true)) {
                e->returnFromChooser=false;e->initialFocusPending=false;
                if(returning||e->pendingReturnMessage.isNotEmpty()) e->publishReturnedFocus(target);
            } else e->initialFocusPending=true;
        }});
    }
    void showGrid(bool show,juce::Component* destination=nullptr) {
        gridOpen=show; search.reset();
        const auto main=std::array<juce::Component*,19>{&open,&parameters,&channel,&bank,&program,&level,&channelLabel,&bankLabel,&programLabel,&levelLabel,&levelReadout,&channelGainLabel,&routingLabel,&presetsButton,&saveButton,&saveAsButton,&aboutButton,&exportButton,&instrumentButton};
        const auto panel=std::array<juce::Component*,6>{&grid,&value,&closeGrid,&gridTitle,&hint,&valueReadout};
        // Reveal the destination before hiding the old focused controls.
        if(show) for(auto* c:panel) c->setVisible(true);
        else { for(auto* c:main) c->setVisible(true); for(auto& c:channelLevels) c.setVisible(true); for(auto& c:routeLevels) c.setVisible(true); }
        resized();
        if(show) { if(hasKeyboardFocus(true)) focus(grid); for(auto* c:main) c->setVisible(false); for(auto& c:channelLevels) c.setVisible(false); for(auto& c:routeLevels) c.setVisible(false); }
        else { if(grid.hasKeyboardFocus(true)||value.hasKeyboardFocus(true)||closeGrid.hasKeyboardFocus(true)) focus(destination!=nullptr?*destination:static_cast<juce::Component&>(parameters)); for(auto* c:panel) c->setVisible(false); }
        repaint();
    }
    void openParameters() {
        if(!gridOpen) { const int id=focusId(juce::Component::getCurrentlyFocusedComponent());proc.editorMemory.gridReturn=id&&!isGridFocus(id)?id:2; refresh(false); rebuildGrid(); showGrid(true); }
        initialFocusPending=false; returnFromChooser=false; focus(grid);
    }
    void closeParameters() { if(!gridOpen) return; restoreFocus(proc.editorMemory.gridReturn); }
    int selectedChannel() const { return juce::jlimit(0,15,channel.getSelectedId()-1); }
    bool isSfzChannel(int ch) const { const auto* p=findPreset(cached.presets,proc.current(ch));return p&&p->sfzIndex>=0; }
    bool isCurrentSfz() const { return isSfzChannel(selectedChannel()); }
    bool currentProgramReady() const { return cached.ready&&!cached.loading&&findPreset(cached.presets,proc.current(selectedChannel()))!=nullptr; }
    const GridRow* selectedRow() const { const int i=grid.getSelectedItemIndex(); return i>=0&&i<static_cast<int>(rows.size())?&rows[static_cast<size_t>(i)]:nullptr; }
    void rebuildGrid() {
        const int preferred=currentProgramReady()?(isCurrentSfz()?lastSfzRow:lastSf2Row):proc.editorMemory.emptyRow;
        gridShownCode=proc.current(selectedChannel());
        rows.clear();
        rows.push_back({rowGain,"Output gain",-60,6,0.1,false," dB"});
        rows.push_back({rowRoute,"Output routing",0,stereoBuses-1,1,true,{}});
        if(currentProgramReady()) {
            if(isCurrentSfz()) {
                rows.push_back({rowInstrumentGain,"SFZ instrument gain",sfzGainMin,sfzGainMax,0.1,false," dB"});
                rows.push_back({rowSfzQuality,"SFZ live sample quality",0,11,1,true,{}});
                if(const auto* p=findPreset(cached.presets,proc.current(selectedChannel()))) {
                    if(const auto it=cached.labels.find(p->path);it!=cached.labels.end())
                        for(const auto& cc:it->second) rows.push_back({cc.cc,cc.name+" (CC"+juce::String(cc.cc)+")",0,127,1,false,{}});
                }
            } else {
                rows.push_back({rowChannel,"MIDI channel",1,16,1,true,{}});
                rows.push_back({rowInterpolation,"SF2 interpolation",0,2,1,true,{}});

                for(const auto& c:sf2Controls) rows.push_back({c.id,c.name,static_cast<double>(c.lo),static_cast<double>(c.hi),static_cast<double>(c.step),false,c.unit});
            }
        }
        if(currentProgramReady()&&(isCurrentSfz()||sf2MultiOutput)) {
            for(int i=0;i<lsfx::compCount;++i) {const auto& c=lsfx::compSpecs[i]; rows.push_back({compRow(i),c.name,c.lo,c.hi,c.step,i==lsfx::compRatio,c.unit});}
        }
        if(currentProgramReady()) {
            rows.push_back({rowEffects,"Effects",0,3,1,true,{}});
            if(!isCurrentSfz()) rows.push_back({rowMidiFX,"MIDI effects reception",0,1,1,true,{}});
            for(int i=0;i<fxCount;++i) { if(i==drumTrim||(isCurrentSfz()&&(i==sf2Dynamics||i==sf2Ambience))) continue; const auto& f=fxSpecs[i]; rows.push_back({fxRow(i),f.label,double(f.lo),double(f.hi),1,i==rvType||i==chType||i==delayMode||i==sf2Dynamics||i==sf2Ambience,f.unit}); }
        }
        gridTitle.setText(currentProgramReady()?(juce::String(isCurrentSfz()?"SFZ parameters - channel ":"SF2 parameters - channel ")+juce::String(selectedChannel()+1)):juce::String("Parameters"),juce::dontSendNotification);
        updatingGrid=true; grid.clear(juce::dontSendNotification);
        int target=0;
        for(size_t i=0;i<rows.size();++i) { if(rows[i].id==preferred) target=static_cast<int>(i); grid.addItem(rowLine(rows[i]),static_cast<int>(i)+1); }
        grid.setSelectedItemIndex(target,juce::dontSendNotification); updatingGrid=false; search.reset(); syncValue(); repaint();
    }
    double readRow(const GridRow& r) const {
        if(const int i=compIndex(r.id);i>=0) return proc.compressorValue(selectedChannel(),i);
        if(const int i=fxIndex(r.id);i>=0) { auto& p=proc.fxParameter(i); return p.convertFrom0to1(p.getValue()); }
        if(r.id==rowGain) return proc.channelGainParameter(selectedChannel()).get();
        if(r.id==rowInstrumentGain) return proc.instrumentGain(proc.current(selectedChannel()));
        if(r.id==rowRoute) return proc.routeParameter(selectedChannel()).getIndex();
        if(r.id==rowInterpolation) return proc.qualityParameter().getIndex();
        if(r.id==rowEffects) return proc.effectsParameter().getIndex();
        if(r.id==rowMidiFX) return proc.midiFXParameter().getIndex();
        if(r.id==rowSfzQuality) return proc.sfzQualityParameter().getIndex();
        if(r.id==rowChannel) return selectedChannel()+1;
        return currentProgramReady()?proc.getControl(selectedChannel(),r.id):unset;
    }
    juce::String formatValue(const GridRow& r,double v) const {
        if(v==unset) return "Instrument default (not read)";
        if(const int i=compIndex(r.id);i>=0) {
            if(i==lsfx::compRatio) return lsfx::compRatioNames[juce::jlimit(0,3,juce::roundToInt(v))];
            return juce::String(v,(i==lsfx::compThreshold||i==lsfx::compMakeup||i==lsfx::compMix)?1:0)+r.unit;
        }
        if(const int i=fxIndex(r.id);i>=0) {
            const int k=juce::jlimit(fxSpecs[i].lo,fxSpecs[i].hi,juce::roundToInt(v));
            if(i==rvType) return reverbNames[k]; if(i==chType) return chorusNames[k]; if(i==delayMode) return k?"Ping-pong":"Stereo";
            if(i==chRate) return juce::String(k*.01,2)+" Hz";
            if(i==sf2Dynamics) return k==2?"Reference (unprocessed)":k==1?"Linear (soft ceiling)":"LJuno Glue";
            if(i==sf2Ambience) return k?"Scoring ambience (CC91 40)":"Native MIDI";
        }
        if(r.id==rowGain||r.id==rowInstrumentGain) return juce::String(v,1)+r.unit;
        if(r.id==rowRoute) return routingValue(v);
        if(r.id==rowInterpolation) return proc.qualityParameter().choices[juce::jlimit(0,2,juce::roundToInt(v))];
        if(r.id==rowMidiFX) return proc.midiFXParameter().choices[juce::jlimit(0,1,juce::roundToInt(v))];
        if(r.id==rowEffects) return proc.effectsParameter().choices[juce::jlimit(0,3,juce::roundToInt(v))];
        if(r.id==rowSfzQuality) return proc.sfzQualityParameter().choices[juce::jlimit(0,11,juce::roundToInt(v))];
        return juce::String(juce::roundToInt(v))+r.unit;
    }
    juce::String routingValue(double v,int ch=-1) const {
        const auto name=routeName(juce::roundToInt(v));
        if(!sf2MultiOutput&&!isSfzChannel(ch<0?selectedChannel():ch)&&v!=0) return name+" (stored; SF2 Main 1/2 only)";
        return name;
    }
    juce::String rowName(const GridRow& r) const { return r.id==rowGain||r.id==rowRoute||compIndex(r.id)>=0?r.name+" channel "+juce::String(selectedChannel()+1):r.name; }
    juce::String rowLine(const GridRow& r) const { return rowName(r)+" "+formatValue(r,readRow(r)); }
    void writeRow(const GridRow& r,double v) {
        v=juce::jlimit(r.low,r.high,r.low+std::round((v-r.low)/r.step)*r.step);
        auto set=[v](juce::RangedAudioParameter& p){ p.beginChangeGesture(); p.setValueNotifyingHost(p.convertTo0to1(static_cast<float>(v))); p.endChangeGesture(); };
        if(const int c=compIndex(r.id);c>=0) set(proc.compressorParameter(selectedChannel(),c));
        else if(const int i=fxIndex(r.id);i>=0) set(proc.fxParameter(i));
        else if(r.id==rowGain) set(proc.channelGainParameter(selectedChannel()));
        else if(r.id==rowInstrumentGain) {if(currentProgramReady()) proc.setInstrumentGain(proc.current(selectedChannel()),float(v));}
        else if(r.id==rowRoute) set(proc.routeParameter(selectedChannel()));
        else if(r.id==rowInterpolation) set(proc.qualityParameter());
        else if(r.id==rowEffects) set(proc.effectsParameter());
        else if(r.id==rowMidiFX) set(proc.midiFXParameter());
        else if(r.id==rowSfzQuality) set(proc.sfzQualityParameter());
        else if(r.id==rowChannel) { proc.setUiChannel(juce::roundToInt(v)-1); channel.setSelectedId(juce::roundToInt(v),juce::dontSendNotification); shownCode=-1; refresh(true); gridTitle.setText("SF2 parameters - channel "+juce::String(selectedChannel()+1),juce::dontSendNotification); }
        else proc.setControl(selectedChannel(),r.id,juce::roundToInt(v));
    }
    void syncValue() {
        if(auto* row=selectedRow()) {
            const auto r=*row;
            if(currentProgramReady()) {if(isCurrentSfz()) lastSfzRow=r.id; else lastSf2Row=r.id;}
            else if(!cached.loading) proc.editorMemory.emptyRow=r.id;
            proc.editorMemory.sf2Row=lastSf2Row; proc.editorMemory.sfzRow=lastSfzRow;
            updatingGrid=true;
            value.setTitle(rowName(r)); value.setName(rowName(r)); value.setRange(r.low,r.high,r.step);
            value.textFromValueFunction=[this,r](double v) { return formatValue(r,(r.id>=0&&readRow(r)==unset&&v==r.low)?static_cast<double>(unset):v); };
            const auto v=readRow(r); value.setValue(v==unset?r.low:v,juce::dontSendNotification); value.updateText(); valueReadout.setText(formatValue(r,v),juce::dontSendNotification);
            updatingGrid=false;
        }
    }
    void refreshGridValues() {
        if(!gridOpen) return;
        const int selectedId=grid.getSelectedId();
        bool changed=false; updatingGrid=true;
        for(size_t i=0;i<rows.size();++i) { const auto text=rowLine(rows[i]); if(grid.getItemText(static_cast<int>(i))!=text) {grid.changeItemText(static_cast<int>(i)+1,text); changed=true;} }
        if(selectedId>0 && grid.getSelectedId()!=selectedId) grid.setSelectedId(selectedId,juce::dontSendNotification);
        if(auto* r=selectedRow()) {
            if(value.getTitle()!=rowName(*r)) {value.setTitle(rowName(*r));value.setName(rowName(*r));}
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
            const auto r=*row; if(r.id>=0&&!currentProgramReady()) {speak(source,"Load an instrument first");return;} double v=readRow(r); if(v==unset) v=r.low;
            const double delta=r.step*(r.choice?1:stepWidths[stepIndex]*(coarse?pageMultiplier:1))*direction;
            const double previous=readRow(r);
            const double next=juce::jlimit(r.low,r.high,boundary?(direction>0?r.high:r.low):v+delta);
            if(previous!=unset&&std::abs(next-previous)<0.000001) return;
            writeRow(r,next); refreshGridValues();
            const double actual=readRow(r);
            if(actual!=previous) speak(source,formatValue(r,actual));
        }
    }
    void changeStep(juce::Component& source,int d) { const int next=juce::jlimit(0,4,stepIndex+d); if(next==stepIndex) return; stepIndex=next; proc.editorMemory.step=next; speak(source,"Step "+juce::String(stepWidths[stepIndex])); }
    bool keyPressed(const juce::KeyPress& key,juce::Component* origin) override { return handleKey(key,origin); }
    juce::Component* keyboardControl(juce::Component* origin) {
        // Some JUCE controls forward a key from their internal label, rather
        // than the slider/combobox itself. Treat both routes identically.
        for(auto* c=origin;c!=nullptr&&c!=this;c=c->getParentComponent()) {
            for(auto& gainControl:channelLevels) if(c==&gainControl) return c;
            for(auto& routeControl:routeLevels) if(c==&routeControl) return c;
            for(auto* target:std::array<juce::Component*,9>{&grid,&value,&level,&open,&parameters,&channel,&bank,&program,&closeGrid}) if(c==target) return target;
        }
        return origin;
    }
    int outputChannel(juce::Component* c) const { for(int i=0;i<channels;++i) if(c==&channelLevels[i]) return i; return -1; }
    void changeChannelLevel(int ch,int direction,bool coarse,bool boundary) {
        auto& c=channelLevels[ch];
        const double next=juce::jlimit(c.getMinimum(),c.getMaximum(),boundary?(direction>0?c.getMaximum():c.getMinimum()):c.getValue()+0.1*stepWidths[stepIndex]*(coarse?pageMultiplier:1)*direction);
        if(std::abs(next-c.getValue())<0.000001) return;
        c.setValue(next,juce::sendNotificationSync); speak(c,juce::String(c.getValue(),1)+" dB");
    }
    void changeMainLevel(int direction,bool coarse,bool boundary=false) {
        const double next=boundary?(direction>0?level.getMaximum():level.getMinimum()):level.getValue()+0.1*stepWidths[stepIndex]*(coarse?pageMultiplier:1)*direction;
        const double clamped=juce::jlimit(level.getMinimum(),level.getMaximum(),next);
        if(std::abs(clamped-level.getValue())<0.000001) return;
        level.setValue(clamped,juce::sendNotificationSync);
        levelReadout.setText(juce::String(level.getValue(),1)+" dB",juce::dontSendNotification);
        speak(level,levelReadout.getText());
    }
    static bool gridMoveForKey(int code,lsfx::GridMove& move) {
        using Move=lsfx::GridMove;
        if(code==juce::KeyPress::upKey) move=Move::up;
        else if(code==juce::KeyPress::downKey) move=Move::down;
        else if(code==juce::KeyPress::leftKey) move=Move::left;
        else if(code==juce::KeyPress::rightKey) move=Move::right;
        else if(code==juce::KeyPress::pageUpKey) move=Move::pageUp;
        else if(code==juce::KeyPress::pageDownKey) move=Move::pageDown;
        else if(code==juce::KeyPress::homeKey) move=Move::home;
        else if(code==juce::KeyPress::endKey) move=Move::end;
        else return false;
        return true;
    }
    void navigateGrid(int code,bool wholeGrid=false) {
        if(rows.empty()) return;
        const int i=juce::jmax(0,grid.getSelectedItemIndex()), count=static_cast<int>(rows.size());
        lsfx::GridMove move;
        if(!gridMoveForKey(code,move)) return;
        const int target=lsfx::gridDestination(i,count,gridRowsPerColumn,move,wholeGrid);
        if(target!=i) selectGridRow(target,true);
    }
    // The existing accessible bank/program controls act as column-major
    // keyboard grids, with no keyboard popup and no wrap at an edge.
    void navigateLocation(juce::ComboBox& control,int code,bool wholeGrid) {
        lsfx::GridMove move;
        if(!gridMoveForKey(code,move)||control.getNumItems()==0) return;
        const int current=juce::jmax(0,control.getSelectedItemIndex());
        const int target=lsfx::gridDestination(current,control.getNumItems(),lsfx::locationGridRows,move,wholeGrid);
        if(target==current) return;
        control.setSelectedItemIndex(target,juce::sendNotificationSync);
        speak(control,control.getText());
    }
    bool handleKey(const juce::KeyPress& k,juce::Component* origin) {
        if(panelMode==Panel::instrument) return false; // editor owns its own keyboard context
        origin=keyboardControl(origin);
        if(returnFromChooser&&!chooser&&!importInProgress) {
            if(focusId(origin)!=chooserReturn) {
                restoreFocus(chooserReturn);
                auto& target=focusTarget(chooserReturn);
                if(target.hasKeyboardFocus(true)) {returnFromChooser=false;initialFocusPending=false;publishReturnedFocus(target);}
                return true;
            }
            returnFromChooser=false; initialFocusPending=false; rememberFocus(focusId(origin));
            publishReturnedFocus(focusTarget(chooserReturn));
        }
        const auto code=k.getKeyCode(); const auto mods=k.getModifiers();
        const auto ch=juce::CharacterFunctions::toLowerCase(code>='A'&&code<='Z'?static_cast<juce::juce_wchar>(code):k.getTextCharacter()!=0?k.getTextCharacter():static_cast<juce::juce_wchar>(code));
        const bool alt=mods.isAltDown(), ctrl=mods.isCtrlDown()||mods.isCommandDown();
        const bool vertical=code==juce::KeyPress::upKey||code==juce::KeyPress::downKey;
        const bool horizontal=code==juce::KeyPress::leftKey||code==juce::KeyPress::rightKey;
        const bool page=code==juce::KeyPress::pageUpKey||code==juce::KeyPress::pageDownKey;
        const bool boundary=code==juce::KeyPress::homeKey||code==juce::KeyPress::endKey;
        const int direction=code==juce::KeyPress::upKey||code==juce::KeyPress::pageUpKey||code==juce::KeyPress::homeKey||code==juce::KeyPress::rightKey?1:-1;
        const int outputCh=outputChannel(origin);
        int routeCh=-1; for(int i=0;i<channels;++i) if(origin==&routeLevels[i]) routeCh=i;
        if(routeCh>=0) lastRouteChannel=routeCh;
        if(outputCh>=0) lastGainChannel=outputCh;
        // Leave the host and the native combo popup in control of their own keys.
        if(chooser||catalogBusy) return false;
        if(ctrl&&!alt&&!mods.isShiftDown()&&code==juce::KeyPress::returnKey&&(panelMode==Panel::none||panelMode==Panel::notes)) {proc.allOff();if(origin)speak(*origin,"All sounds off");return true;}
        if(ctrl&&alt&&!mods.isShiftDown()&&(ch=='m'||code=='M'||code=='m')&&(panelMode==Panel::none||panelMode==Panel::notes)) {
            const bool accepted=proc.resetMidi();
            if(origin) speak(*origin,accepted?"MIDI reset":"MIDI reset unavailable while empty or loading");
            return true;
        }
        if(panelMode==Panel::notes) return handleNoteKey(k);
        if(panelMode==Panel::about) return handleAboutKey(k,origin);
        if(panelMode!=Panel::none) return handlePanelKey(k,origin);
        if(importInProgress) return true;
        if(cached.loading&&(origin==&bank||origin==&program||origin==&channel)&&!alt&&!ctrl&&(vertical||horizontal||page||boundary||code==juce::KeyPress::returnKey||juce::CharacterFunctions::isLetterOrDigit(k.getTextCharacter()))) return true;
        initialFocusPending=false; rememberFocus(focusId(origin));
        if(!gridOpen&&(bank.isPopupActive()||program.isPopupActive()||channel.isPopupActive())) return false;
        if(ctrl&&!alt&&(ch=='s'||code==19||k.getTextCharacter()==19)) {if(mods.isShiftDown()) showSaveAs(); else saveCurrent();return true;}
        if(gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::tabKey) {
            if(origin==&grid) focus(mods.isShiftDown()?static_cast<juce::Component&>(closeGrid):static_cast<juce::Component&>(value));
            else if(origin==&value) focus(mods.isShiftDown()?static_cast<juce::Component&>(grid):static_cast<juce::Component&>(closeGrid));
            else focus(mods.isShiftDown()?static_cast<juce::Component&>(value):static_cast<juce::Component&>(grid));
            return true;
        }
        // The channel gain strip is one logical Tab stop, even though each
        // fader remains a native accessible control for arrows and mouse.
        if(!gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::tabKey) {
            const bool backwards=mods.isShiftDown();
            auto& remembered=channelLevels[lastGainChannel];
            if(outputCh>=0) {
                if(backwards) focus(channel.isEnabled()?static_cast<juce::Component&>(channel):static_cast<juce::Component&>(parameters));
                else focus(routeLevels[lastRouteChannel]);
                return true;
            }
            if(routeCh>=0) { if(backwards) focus(remembered); else focus(level); return true; }
            if(backwards&&origin==&level) { focus(routeLevels[lastRouteChannel]); return true; }
            if(!backwards&&(origin==&channel||(origin==&parameters&&!channel.isEnabled()))) {
                focus(remembered);
                return true;
            }
        }

        if(alt&&!ctrl&&!mods.isShiftDown()) {
            search.reset();
            if(gridOpen&&(origin==&grid||origin==&value)) {
                if(horizontal) {changeStep(*origin,direction);return true;}
                if(vertical||page||boundary) {editSelected(*origin,direction,page,boundary);return true;}
            }
            if(!gridOpen&&outputCh>=0) {
                if(horizontal) {changeStep(*origin,direction);return true;}
                if(vertical||page||boundary) {changeChannelLevel(outputCh,direction,page,boundary);return true;}
            }
            if(!gridOpen&&origin==&level) {
                if(horizontal) {changeStep(level,direction);return true;}
                if(vertical||page||boundary) {changeMainLevel(direction,page,boundary);return true;}
            }
            if(ch=='i') { requestInitialize(origin); return true; }
            if(code==juce::KeyPress::returnKey) {openNoteGrid();return true;}
            if(ch=='a') {openAbout(); return true;}
            if(ch=='e') {browseExport();return true;}
            if(ch=='t') {openInstrumentEditor();return true;}
            if(ch=='s') {showStatus(); return true;}
            if(ch=='l') {openParameters();return true;}
            if(ch=='o') {browse();return true;}
            if(ch=='b') {openPresetBrowser();return true;}
            if(ch=='v'&&gridOpen) {focus(value);return true;}
            if(ch=='c'&&gridOpen) {closeParameters();return true;}
            juce::Component* target=nullptr;
            if(ch=='m') target=&channel; else if(ch=='k') target=&bank; else if(ch=='p') target=&program; else if(ch=='g') target=&level; else if(ch=='u') target=&channelLevels[lastGainChannel]; else if(ch=='r') target=&routeLevels[lastRouteChannel];
            if(target) {if(target->isEnabled()) {if(gridOpen) showGrid(false,target); focus(*target);} return true;}
        }
        if(!gridOpen&&!alt&&!mods.isShiftDown()&&(origin==&bank||origin==&program)
            &&((!ctrl&&(vertical||horizontal||page||boundary))||(ctrl&&boundary))) {
            if(!cached.loading) navigateLocation(origin==&bank?static_cast<juce::ComboBox&>(bank):static_cast<juce::ComboBox&>(program),code,ctrl);
            return true;
        }
        if(!gridOpen&&!alt&&!ctrl&&!mods.isShiftDown()&&ch>='1'&&ch<='9'
            &&(origin==&channel||outputCh>=0||routeCh>=0)) {
            const int current=origin==&channel?selectedChannel():outputCh>=0?outputCh:routeCh;
            const int next=lsfx::channelDigitDestination(current,int(ch-'0'),channels);
            if(next!=current) {
                if(origin==&channel) {
                    if(!cached.loading) {channel.setSelectedId(next+1,juce::sendNotificationSync);speak(channel,channel.getText());}
                } else if(outputCh>=0) {lastGainChannel=next;focus(channelLevels[next]);}
                else {lastRouteChannel=next;focus(routeLevels[next]);}
            }
            return true;
        }
        if(!gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::deleteKey&&(origin==&bank||origin==&program)) {
            if(!cached.loading) requestClearLocation(origin==&bank);return true;
        }
        if(!gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::returnKey&&(origin==&bank||origin==&program)) {
            if(!cached.loading) {if(origin==&bank) browseImport(false);else browseImport(true);} return true;
        }
        if(gridOpen&&!alt&&!ctrl&&code==juce::KeyPress::escapeKey) {closeParameters();return true;}
        if(alt||ctrl) {
            if(!alt&&ctrl&&gridOpen&&origin==&grid&&boundary) {search.reset();navigateGrid(code,true);return true;}
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
                    if(const int c=compIndex(copy.id);c>=0) reset=lsfx::compSpecs[c].initial;
                    else if(f>=0) reset=fxSpecs[f].initial;
                    else if(copy.id>=128) reset=0;
                    else if(copy.id==rowGain||copy.id==rowRoute||copy.id==rowInstrumentGain) reset=0;
                    else if(copy.id==rowInterpolation) reset=1;
                    else if(copy.id==rowEffects||copy.id==rowSfzQuality) reset=0;
                    else if(copy.id==rowMidiFX) reset=1;
                    else supported=false;
                    if(supported) {const auto before=readRow(copy); if(std::abs(before-reset)>0.000001) {writeRow(copy,reset);refreshGridValues(); const auto after=readRow(copy); if(after!=before) speak(*origin,formatValue(copy,after));}}
                }
                return true;
            }
        }
        if(routeCh>=0) {
            if(horizontal) {const int next=juce::jlimit(0,channels-1,routeCh+direction); if(next!=routeCh) {lastRouteChannel=next;focus(routeLevels[next]);} return true;}
            if(vertical||page||boundary||code==juce::KeyPress::backspaceKey) {
                auto& c=routeLevels[routeCh]; const int old=juce::roundToInt(c.getValue());
                const int next=code==juce::KeyPress::backspaceKey?0:code==juce::KeyPress::homeKey?0:code==juce::KeyPress::endKey?stereoBuses-1:juce::jlimit(0,stereoBuses-1,old+direction*(page?4:1));
                if(next!=old) {c.setValue(next,juce::sendNotificationSync);speak(c,routingValue(next,routeCh));} return true;
            }
        }
        if(outputCh>=0) {
            if(horizontal) {const int next=juce::jlimit(0,channels-1,outputCh+direction); if(next!=outputCh) {lastGainChannel=next;focus(channelLevels[next]);} return true;}
            if(vertical||page||boundary) {changeChannelLevel(outputCh,direction,page,boundary);return true;}
            if(code==juce::KeyPress::backspaceKey) {if(channelLevels[outputCh].getValue()!=0) {channelLevels[outputCh].setValue(0,juce::sendNotificationSync);speak(*origin,"0.0 dB");}return true;}
        }
        if(origin==&level) {
            if(horizontal) {changeStep(level,direction);return true;}
            if(vertical||page||boundary) {changeMainLevel(direction,page,boundary);return true;}
        }
        if(dynamic_cast<juce::Button*>(origin)&&(horizontal||vertical||page||boundary)) return true;
        return false;
    }
    void announce(const juce::String& text) {
        if(auto* c=juce::Component::getCurrentlyFocusedComponent(); c!=nullptr&&isParentOf(c)) speak(*c,text);
        // No speech if there is no focused accessible control.
    }
    // About panel and caret handling adapted from LJuno-116 (AGPL-3.0-or-later).
    void openAbout() {
        if(panelMode!=Panel::none) return;
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::about;
        for(auto* c:std::array<juce::Component*,4>{&aboutInfo,&aboutProject,&aboutContact,&aboutClose}) {c->setVisible(true);c->toFront(false);}
        resized(); repaint(); aboutInfo.setCaretPosition(0);
        juce::AccessibilityHandler::clearCurrentlyFocusedHandler(); aboutInfo.grabKeyboardFocus();
        if(auto* h=aboutInfo.getAccessibilityHandler()) h->grabFocus();
    }
    void openAboutLink(bool email) {
        auto& control=email?aboutContact:aboutProject;
        auto url=email?juce::String("mailto:")+lsfxContactEmail:juce::String(lsfxProjectUrl);
        if(!juce::URL(url).launchInDefaultBrowser()) speak(control,email?"Cannot open the email application":"Cannot open the LSFX GitHub project page");
    }
    bool handleAboutKey(const juce::KeyPress& key,juce::Component* origin) {
        const auto code=key.getKeyCode(); const auto mods=key.getModifiers();
        const auto ch=juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code));
        if(code==juce::KeyPress::escapeKey||(mods.isAltDown()&&!mods.isCtrlDown()&&ch=='c')) {closePanel();return true;}
        if(code==juce::KeyPress::tabKey) {
            std::array<juce::Component*,4> controls{&aboutInfo,&aboutProject,&aboutContact,&aboutClose};
            int i=0; for(int k=0;k<4;++k) if(controls[k]->hasKeyboardFocus(true)) {i=k;break;}
            focus(*controls[(i+(mods.isShiftDown()?3:1))%4]); return true;
        }
        if(aboutInfo.hasKeyboardFocus(true)&&navigateAboutText(key)) return true;
        if(code==juce::KeyPress::returnKey) {
            if(origin==&aboutProject) openAboutLink(false);
            else if(origin==&aboutContact) openAboutLink(true);
            else closePanel(); return true;
        }
        if(key.getTextCharacter()==' '&&origin!=&aboutInfo) {
            if(origin==&aboutProject) openAboutLink(false); else if(origin==&aboutContact) openAboutLink(true); else closePanel(); return true;
        }
        return false;
    }
    bool navigateAboutText (const juce::KeyPress& key)
    {
        const auto keyCode = key.getKeyCode();
        const auto modifiers = key.getModifiers();
        const bool selecting = modifiers.isShiftDown();
        const bool byWordOrDocument = modifiers.isCtrlDown() || modifiers.isCommandDown();

        // TextEditor caret/selection methods already emit the accessibility
        // selection events used by NVDA, JAWS and Narrator.  Do not add a manual
        // spoken announcement here: doing both is what caused each character to be
        // spoken twice in the About panel.
        if (keyCode == juce::KeyPress::leftKey)
        {
            aboutInfo.moveCaretLeft (byWordOrDocument, selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::rightKey)
        {
            aboutInfo.moveCaretRight (byWordOrDocument, selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::upKey)
        {
            if (byWordOrDocument && selecting)
                aboutInfo.moveCaretToTop (true);
            else
                aboutInfo.moveCaretUp (selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::downKey)
        {
            if (byWordOrDocument && selecting)
                aboutInfo.moveCaretToEnd (true);
            else
                aboutInfo.moveCaretDown (selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::homeKey)
        {
            if (byWordOrDocument)
                aboutInfo.moveCaretToTop (selecting);
            else
                aboutInfo.moveCaretToStartOfLine (selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::endKey)
        {
            if (byWordOrDocument)
                aboutInfo.moveCaretToEnd (selecting);
            else
                aboutInfo.moveCaretToEndOfLine (selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::pageUpKey)
        {
            aboutInfo.pageUp (selecting);
            return true;
        }

        if (keyCode == juce::KeyPress::pageDownKey)
        {
            aboutInfo.pageDown (selecting);
            return true;
        }

        const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase (
            static_cast<juce::juce_wchar> (keyCode));

        if (byWordOrDocument && lowerKeyCode == 'a')
        {
            aboutInfo.selectAll();
            return true;
        }

        if (byWordOrDocument && lowerKeyCode == 'c')
        {
            aboutInfo.copyToClipboard();
            return true;
        }

        return false;
    }
    void openInstrumentEditor() {
        if(chooser||catalogBusy||importInProgress||panelMode!=Panel::none||cached.loading)return;
        const int ch=selectedChannel(),code=proc.current(ch);
        auto expected=std::make_shared<std::optional<Preset>>();
        const auto initialView=proc.getView();if(const auto* current=findPreset(initialView.presets,code))*expected=*current;
        const auto address="Channel "+juce::String(ch+1)+", Bank "+number(code>>7)+", Program "+number(code&127);
        hideMainForPanel();hidePanelControls();panelMode=Panel::instrument;
        instrumentPanel=std::make_unique<InstrumentEditorPanel>(proc.instrumentDraft,[this]{
            // Avoid destroying the focused component in its own key callback.
            juce::Component::SafePointer<Editor> safe(this);
            juce::MessageManager::callAsync([safe]{if(auto* e=safe.getComponent();e&&e->panelMode==Panel::instrument){e->proc.stopInstrumentPreview();e->instrumentPanel.reset();e->closePanel(&e->instrumentButton);e->refresh(true);}});
        },[this,ch,code,expected](const juce::File& file){
            const auto view=proc.getView();const auto* current=findPreset(view.presets,code);
            const bool same=expected->has_value()?(current&&current->path==expected->value().path&&current->sourceBank==expected->value().sourceBank&&current->sourceProgram==expected->value().sourceProgram):current==nullptr;
            if(view.loading||proc.current(ch)!=code||!same)return juce::Result::fail("MIDI destination changed or is loading. File exported; load it manually.");
            auto result=file.hasFileExtension("sf2")?proc.importSF2(file,code>>7,code&127,0,0,ch):proc.importSFZ(file,code,ch);
            if(result.wasOk())*expected=Preset{code>>7,code&127,file.getFileNameWithoutExtension(),file.getFullPathName(),0,0};
            return result;
        },address,[this](const lsfx::instrument::Project& p,int raw){proc.previewInstrument(p,raw);},[this]{return proc.instrumentPreview.getStatus();},[this]{proc.instrumentPreview.panic();});
        addAndMakeVisible(*instrumentPanel);resized();repaint();instrumentPanel->activate();
    }
    void hidePanelControls() {
        panelCancel.setDescription({});
        for(auto* c:std::array<juce::Component*,11>{&presetList,&parentButton,&panelOK,&panelCancel,&panelText,&panelLabel,&aboutInfo,&aboutProject,&aboutContact,&aboutClose,&noteCell}) c->setVisible(false);
    }
    void hideMainForPanel() {
        if(panelMode==Panel::none) {const int id=focusId(juce::Component::getCurrentlyFocusedComponent());panelReturn=id?id:proc.editorMemory.focus;}
        juce::ScopedValueSetter<bool> guard(focusTransition,true);
        if(gridOpen) showGrid(false);
        for(auto* c:std::array<juce::Component*,19>{&open,&parameters,&channel,&bank,&program,&level,&channelLabel,&bankLabel,&programLabel,&levelLabel,&levelReadout,&channelGainLabel,&routingLabel,&presetsButton,&saveButton,&saveAsButton,&aboutButton,&exportButton,&instrumentButton}) c->setVisible(false);
        for(auto& c:channelLevels) c.setVisible(false);
        for(auto& c:routeLevels) c.setVisible(false);
        initialFocusPending=false; returnFromChooser=false;
    }
    void closePanel(juce::Component* destination=nullptr) {
        juce::ScopedValueSetter<bool> guard(focusTransition,true);
        if(panelMode==Panel::notes) {proc.setNoteLearnChannel(-1);startTimerHz(5);}
        panelMode=Panel::none; panelAction={}; panelCancelAction={}; sourceAction={}; sourceEntries.clear();
        hidePanelControls(); showGrid(false); initialFocusPending=false; returnFromChooser=false;
        restoreFocus(destination?focusId(destination):panelReturn);
    }
    void cancelPanel() {
        if(panelMode==Panel::none) return;
        if(panelMode==Panel::notes) {closePanel();return;}
        if(panelMode==Panel::exportProgress&&exportJob) {if(!exportJob->done.load()) {exportJob->cancel.store(true); setPanelLabel("Cancelling SoundFont export..."); announce("Cancelling SoundFont export");} return;}
        if(panelCancelAction) { auto action=panelCancelAction; action(); }
        else closePanel();
    }
    void setPanelLabel(const juce::String& text) { panelLabel.setText(text,juce::dontSendNotification); panelLabel.setTitle(text); }
    void showNamePanel(const juce::String& prompt,const juce::String& initial,std::function<void()> action,std::function<void()> cancel={}) {
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::name; panelAction=std::move(action); panelCancelAction=std::move(cancel);
        setPanelLabel(prompt); panelText.setTitle(prompt); panelText.setText(initial,false); panelOK.setButtonText("OK"); panelOK.setTitle("OK"); panelCancel.setButtonText("Cancel"); panelCancel.setTitle("Cancel");
        for(auto* c:std::array<juce::Component*,4>{&panelText,&panelLabel,&panelOK,&panelCancel}) c->setVisible(true);
        resized(); repaint(); focus(panelText); panelText.selectAll();
    }
    void showConfirmation(const juce::String& prompt,const juce::String& positive,std::function<void()> action,std::function<void()> cancel={},bool focusPositive=false) {
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::confirm; panelAction=std::move(action); panelCancelAction=std::move(cancel);
        setPanelLabel(prompt); panelOK.setButtonText(positive); panelOK.setTitle(positive); panelCancel.setButtonText("Cancel"); panelCancel.setTitle("Cancel");
        for(auto* c:std::array<juce::Component*,3>{&panelLabel,&panelOK,&panelCancel}) c->setVisible(true);
        resized(); repaint(); focus(focusPositive?static_cast<juce::Component&>(panelOK):static_cast<juce::Component&>(panelCancel)); announce(prompt);
    }
    int getNumRows() override { return static_cast<int>(panelMode==Panel::source?sourceEntries.size():browserEntries.size()); }
    juce::String getNameForRow(int row) override {
        if(!juce::isPositiveAndBelow(row,getNumRows())) return {};
        if(panelMode==Panel::source) return sourceEntries[static_cast<size_t>(row)].second;
        const auto& f=browserEntries[static_cast<size_t>(row)]; return f.isDirectory()?"Folder "+f.getFileName():f.getFileNameWithoutExtension();
    }
    void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool selected) override {
        if(selected) g.fillAll(juce::Colour(0xff304d66));
        g.setColour(juce::Colours::white); g.setFont(juce::Font(juce::FontOptions(15.0f))); g.drawText(getNameForRow(row),8,0,width-16,height,juce::Justification::centredLeft,true);
    }
    void selectedRowsChanged(int row) override { if((panelMode==Panel::browser||panelMode==Panel::source)&&presetList.hasKeyboardFocus(true)&&juce::isPositiveAndBelow(row,getNumRows())) announce(getNameForRow(row)); }
    void listBoxItemDoubleClicked(int row,const juce::MouseEvent&) override { if(panelMode==Panel::browser) activatePreset(row); else if(panelMode==Panel::source) activateSource(row); }
    void returnKeyPressed(int row) override { if(panelMode==Panel::browser) activatePreset(row); else if(panelMode==Panel::source) activateSource(row); }
    void deleteKeyPressed(int) override { if(panelMode==Panel::browser) deletePreset(); }
    juce::File selectedPreset() { const int row=presetList.getSelectedRow(); return juce::isPositiveAndBelow(row,getNumRows())?browserEntries[static_cast<size_t>(row)]:juce::File{}; }
    void refreshBrowser(const juce::File& selected={}) {
        browserEntries.clear();
        if(!insideLibrary(browserDirectory)||!browserDirectory.isDirectory()) browserDirectory=presetRoot();
        for(const auto& f:browserDirectory.findChildFiles(juce::File::findDirectories,false)) if(!f.isHidden()) browserEntries.push_back(f);
        for(const auto& f:browserDirectory.findChildFiles(juce::File::findFiles,false,"*.lsfx")) if(!f.isHidden()) browserEntries.push_back(f);
        std::sort(browserEntries.begin(),browserEntries.end(),[](const juce::File& a,const juce::File& b){if(a.isDirectory()!=b.isDirectory()) return a.isDirectory();return a.getFileName().compareNatural(b.getFileName(),false)<0;});
        setPanelLabel("Preset browser. "+(browserDirectory==presetRoot()?juce::String("LSFX"):browserDirectory.getRelativePathFrom(presetRoot())));
        presetList.updateContent(); presetList.deselectAllRows();
        if(!browserEntries.empty()) {int index=0; for(int i=0;i<getNumRows();++i) if(browserEntries[static_cast<size_t>(i)]==selected) index=i; presetList.selectRow(index); presetList.scrollToEnsureRowIsOnscreen(index);}
    }
    void showBrowserAt(const juce::File& selected={}) {
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::browser; panelAction={}; panelCancelAction={};
        presetList.setName("Preset browser");presetList.setTitle("Preset browser");
        panelCancel.setButtonText("Close");panelCancel.setTitle("Close");
        for(auto* c:std::array<juce::Component*,4>{&presetList,&parentButton,&panelCancel,&panelLabel}) c->setVisible(true);
        refreshBrowser(selected); resized(); repaint(); focus(presetList);
        announce(panelLabel.getText()+". "+(getNumRows()?getNameForRow(presetList.getSelectedRow()):juce::String("Empty folder")));
    }
    void openPresetBrowser() {
        if(panelMode!=Panel::none) return;
        if(auto result=presetRoot().createDirectory(); result.failed()) {announce(result.getErrorMessage());return;}
        const auto last=preset_browser_memory::recalled(presetRoot(),settingsName);
        browserDirectory=last.existsAsFile()?last.getParentDirectory():presetRoot();
        rememberSetting("saveDirectory",browserDirectory.getFullPathName()); showBrowserAt(last);
    }
    void parentFolder() {
        if(browserDirectory==presetRoot()) return;
        const auto child=browserDirectory; browserDirectory=browserDirectory.getParentDirectory();
        rememberSetting("saveDirectory",browserDirectory.getFullPathName()); showBrowserAt(child);
    }
    void activatePreset(int row) {
        if(!juce::isPositiveAndBelow(row,getNumRows())) return;
        const auto f=browserEntries[static_cast<size_t>(row)];
        if(f.isDirectory()) {browserDirectory=f;rememberSetting("saveDirectory",f.getFullPathName());showBrowserAt();return;}
        loadPreset(f);
    }
    void loadPreset(const juce::File& f) {
        juce::MemoryBlock state; juce::String missing;
        const auto result=proc.readPresetFile(f,state,missing);
        if(result.failed()) {
            if(missing.isNotEmpty()) {
                showConfirmation("Instrument file not found: "+missing+". Connect the drive, then Retry or Cancel.","Retry",[this,f]{loadPreset(f);},[this,f]{showBrowserAt(f);},true);
            } else {showBrowserAt(f);announce(result.getErrorMessage());}
            return;
        }
        // Do not change any current parameter or instrument until the referenced file is present.
        pendingBrowserPreset=f;
        proc.setStateInformation(state.getData(),static_cast<int>(state.getSize()));
        rememberSetting("saveDirectory",f.getParentDirectory().getFullPathName());
        closePanel(); announceLoaded=true; refresh(true);
        announce("Preset selected. "+f.getFileNameWithoutExtension());
    }
    void maybePromptMissing() {
        if(panelMode!=Panel::none||chooser||cached.loading||cached.revision==missingPromptRevision||!cached.status.startsWith("SoundFont not found: ")) return;
        if(!isShowing()||getPeer()==nullptr||!getPeer()->isFocused()||!hasKeyboardFocus(true)) return;
        missingPromptRevision=cached.revision;
        const auto missing=cached.status.fromFirstOccurrenceOf("SoundFont not found: ",false,false);
        showConfirmation("Instrument file not found: "+missing+". Connect the drive, then Retry or Cancel.","Retry",[this,missing]{
            if(!juce::File(missing).existsAsFile()) {announce("Instrument file still not found: "+missing);return;}
            // Do not retry a stale request if the host changed state while the panel was open.
            if(proc.getView().status=="SoundFont not found: "+missing) {proc.retryFile();announceLoaded=true;}
            closePanel();refresh(true);
        },[this]{closePanel();},true);
    }
    juce::String legalName(juce::String name) {
        name=juce::File::createLegalFileName(name.trim());
        return name=="."||name==".."?juce::String{}:name;
    }
    void saveTo(const juce::File& f) {
        const auto result=proc.savePresetFile(f);
        if(result.failed()) {announce(result.getErrorMessage());return;}
        rememberSetting("saveDirectory",f.getParentDirectory().getFullPathName());
        if(panelMode!=Panel::none) closePanel();
        announce("Preset saved. "+f.getFileNameWithoutExtension());
    }
    void saveCurrent() {
        const auto f=proc.currentPresetFile();
        if(f.existsAsFile()&&f.isAChildOf(presetRoot())) {saveTo(f);return;}
        showSaveAs();
    }
    void showSaveAs() {
        const auto result=presetRoot().createDirectory(); if(result.failed()) {announce(result.getErrorMessage());return;}
        const auto directory=savedDirectory(); const auto current=proc.currentPresetFile();
        showNamePanel("Preset name. Folder "+directory.getFileName(),current.existsAsFile()?current.getFileNameWithoutExtension():juce::String{},[this,directory]{
            auto name=panelText.getText().trim(); if(name.endsWithIgnoreCase(".lsfx")) name=name.dropLastCharacters(5);
            name=legalName(name); if(name.isEmpty()) {announce("Enter a valid preset name");focus(panelText);return;}
            const auto f=directory.getChildFile(name+".lsfx");
            if(f.existsAsFile()) {showConfirmation("Replace preset "+name+"?","Replace",[this,f]{saveTo(f);},[this]{showSaveAs();});return;}
            saveTo(f);
        });
    }
    void renamePreset() {
        const auto f=selectedPreset(); if(f==juce::File{}) return;
        const bool folder=f.isDirectory();
        showNamePanel(folder?"Folder name":"Preset name",folder?f.getFileName():f.getFileNameWithoutExtension(),[this,f,folder]{
            auto name=panelText.getText().trim(); if(!folder&&name.endsWithIgnoreCase(".lsfx")) name=name.dropLastCharacters(5);
            name=legalName(name); if(name.isEmpty()) {announce("Enter a valid name");return;}
            const auto target=f.getSiblingFile(name+(folder?juce::String{}:juce::String(".lsfx")));
            if(target==f) {showBrowserAt(f);return;}
            if(target.exists()||!f.moveFileTo(target)) {announce("Cannot rename. Destination may already exist.");return;}
            const auto current=proc.currentPresetFile();
            if(current==f) proc.setCurrentPresetFile(target);
            else if(current!=juce::File{}&&current.isAChildOf(f)) proc.setCurrentPresetFile(target.getChildFile(current.getRelativePathFrom(f)));
            preset_browser_memory::renamed(presetRoot(),f,target,settingsName);
            showBrowserAt(target);
        },[this,f]{showBrowserAt(f);});
    }
    void newFolder() {
        showNamePanel("New folder name",{},[this]{
            const auto name=legalName(panelText.getText()); if(name.isEmpty()) {announce("Enter a valid folder name");return;}
            const auto f=browserDirectory.getChildFile(name); if(f.exists()) {announce("Name already exists");return;}
            const auto result=f.createDirectory(); if(result.failed()) {announce(result.getErrorMessage());return;} showBrowserAt(f);
        },[this]{showBrowserAt();});
    }
    void deletePreset() {
        const auto f=selectedPreset(); if(f==juce::File{}) return;
        if(f.isDirectory()) {announce("Only preset files can be deleted here");return;}
        showConfirmation("Delete preset "+f.getFileNameWithoutExtension()+"?","Delete",[this,f]{
            if(!f.deleteFile()) {announce("Cannot delete preset");return;}
            preset_browser_memory::remove(presetRoot(),f,settingsName);
            if(proc.currentPresetFile()==f) proc.setCurrentPresetFile(juce::File{});
            showBrowserAt();
        },[this,f]{showBrowserAt(f);});
    }
    void browserMenu() {
        juce::PopupMenu menu; menu.addItem(1,"Rename",selectedPreset()!=juce::File{}); menu.addItem(2,"New folder"); menu.addItem(3,"Delete preset",selectedPreset().existsAsFile());
        juce::Component::SafePointer<Editor> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetList),[safe](int id){if(auto* e=safe.getComponent();e&&e->panelMode==Panel::browser) {if(id==1)e->renamePreset();else if(id==2)e->newFolder();else if(id==3)e->deletePreset();}});
    }
    bool handlePanelKey(const juce::KeyPress& k,juce::Component*) {
        const auto code=k.getKeyCode();const auto mods=k.getModifiers();const bool ctrl=mods.isCtrlDown()||mods.isCommandDown();
        const auto ch=juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code));
        if(code==juce::KeyPress::escapeKey) {cancelPanel();return true;}
        if(panelMode==Panel::exportProgress) {if(code==juce::KeyPress::returnKey||code==juce::KeyPress::spaceKey)cancelPanel();return true;}
        if(panelMode==Panel::source) {
            if(code==juce::KeyPress::tabKey) {focus(presetList.hasKeyboardFocus(true)?static_cast<juce::Component&>(panelCancel):static_cast<juce::Component&>(presetList));return true;}
            if(code==juce::KeyPress::returnKey) {if(panelCancel.hasKeyboardFocus(true))cancelPanel();else activateSource(presetList.getSelectedRow());return true;}
            const int old=presetList.getSelectedRow(),count=getNumRows(); int next=old;
            if(code==juce::KeyPress::upKey) --next;else if(code==juce::KeyPress::downKey) ++next;else if(code==juce::KeyPress::pageUpKey)next-=10;else if(code==juce::KeyPress::pageDownKey)next+=10;else if(code==juce::KeyPress::homeKey)next=0;else if(code==juce::KeyPress::endKey)next=count-1;
            if(count>0) {next=juce::jlimit(0,count-1,next);if(next!=old){focus(presetList);presetList.selectRow(next);presetList.scrollToEnsureRowIsOnscreen(next);}}
            return true;
        }
        if(panelMode==Panel::browser) {
            if(mods.isAltDown()) {if(ch=='b'||ch=='c')cancelPanel();return true;}
            if(code==juce::KeyPress::tabKey) {
                if(presetList.hasKeyboardFocus(true)) focus(mods.isShiftDown()?static_cast<juce::Component&>(panelCancel):static_cast<juce::Component&>(parentButton));
                else if(parentButton.hasKeyboardFocus(true)) focus(mods.isShiftDown()?static_cast<juce::Component&>(presetList):static_cast<juce::Component&>(panelCancel));
                else focus(mods.isShiftDown()?static_cast<juce::Component&>(parentButton):static_cast<juce::Component&>(presetList));return true;
            }
            if(code==juce::KeyPress::returnKey) {if(panelCancel.hasKeyboardFocus(true))cancelPanel();else if(parentButton.hasKeyboardFocus(true))parentFolder();else activatePreset(presetList.getSelectedRow());return true;}
            if(code==juce::KeyPress::backspaceKey){parentFolder();return true;}
            if(code==juce::KeyPress::F2Key){renamePreset();return true;}
            if(code==juce::KeyPress::deleteKey){deletePreset();return true;}
            if((ctrl&&(ch=='n'||k.getTextCharacter()==14))){newFolder();return true;}
            if((code==juce::KeyPress::F10Key&&mods.isShiftDown())||code==0x5d||code==0x1005d){browserMenu();return true;}
            const int count=getNumRows(), old=presetList.getSelectedRow(); int next=old;
            if(code==juce::KeyPress::upKey) next=old-1;else if(code==juce::KeyPress::downKey)next=old+1;else if(code==juce::KeyPress::pageUpKey)next=old-10;else if(code==juce::KeyPress::pageDownKey)next=old+10;else if(code==juce::KeyPress::homeKey)next=0;else if(code==juce::KeyPress::endKey)next=count-1;
            else if(!ctrl&&count>0&&juce::CharacterFunctions::isLetterOrDigit(k.getTextCharacter())) {const auto wanted=juce::CharacterFunctions::toLowerCase(k.getTextCharacter());for(int i=1;i<=count;++i){const int pos=(old+(mods.isShiftDown()?-i:i)+2*count)%count;const auto name=browserEntries[static_cast<size_t>(pos)].getFileName();if(name.isNotEmpty()&&juce::CharacterFunctions::toLowerCase(name[0])==wanted){next=pos;break;}}}
            if(count>0) {next=juce::jlimit(0,count-1,next);if(next!=old){focus(presetList);presetList.selectRow(next);presetList.scrollToEnsureRowIsOnscreen(next);}}
            return true;
        }
        if(code==juce::KeyPress::tabKey) {
            if(panelMode==Panel::name) {
                if(panelText.hasKeyboardFocus(true))focus(mods.isShiftDown()?static_cast<juce::Component&>(panelCancel):static_cast<juce::Component&>(panelOK));
                else if(panelOK.hasKeyboardFocus(true))focus(mods.isShiftDown()?static_cast<juce::Component&>(panelText):static_cast<juce::Component&>(panelCancel));
                else focus(mods.isShiftDown()?static_cast<juce::Component&>(panelOK):static_cast<juce::Component&>(panelText));
            } else focus(panelOK.hasKeyboardFocus(true)?static_cast<juce::Component&>(panelCancel):static_cast<juce::Component&>(panelOK));
            return true;
        }
        if(code==juce::KeyPress::returnKey||(code==juce::KeyPress::spaceKey&&(panelOK.hasKeyboardFocus(true)||panelCancel.hasKeyboardFocus(true)))) {if(panelCancel.hasKeyboardFocus(true))cancelPanel();else if(panelAction){auto action=panelAction;action();}return true;}
        if(panelMode==Panel::confirm) return true;
        return false;
    }
    static juce::String noteName(int key) {
        return juce::MidiMessage::getMidiNoteName(key,true,true,4)+", MIDI "+juce::String(key);
    }
    static const char* noteColumnName(int col) {
        constexpr const char* names[]={"Output","Level","Pan","Pitch"};return names[juce::jlimit(0,3,col)];
    }
    lsfx::NoteAssignment selectedNoteValue(int key) const {
        const auto it=noteRows.find(key);return it==noteRows.end()?lsfx::NoteAssignment{}:it->second;
    }
    static juce::String noteValue(const lsfx::NoteAssignment& v,int col) {
        switch(col) {
            case 0:return v.output<0?juce::String("Channel Default"):routeName(v.output);
            case 1:return juce::String(double(v.level)/10.0,1)+" dB";
            case 2:return v.pan==0?juce::String("Center"):juce::String(v.pan<0?"Left ":"Right ")+juce::String(std::abs(v.pan))+"%";
            default:return juce::String(v.pitch)+" cents";
        }
    }
    std::vector<int> visibleNoteRows() const {
        std::vector<int> list;for(const auto& item:noteRows)list.push_back(item.first);
        if(noteSelected>=0&&!noteRows.count(noteSelected)) list.insert(std::lower_bound(list.begin(),list.end(),noteSelected),noteSelected);
        return list;
    }
    int notePageStart(const std::vector<int>& list) const {
        const auto i=std::find(list.begin(),list.end(),noteSelected);return i==list.end()?0:(int(i-list.begin())/12)*12;
    }
    void paintNoteGrid(juce::Graphics& g) {
        const auto list=visibleNoteRows();const auto first=notePageStart(list);const int w=noteArea.getWidth()/4;
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        for(int col=0;col<4;++col) {
            auto h=juce::Rectangle<int>(noteArea.getX()+col*w,noteArea.getY(),w,25);
            g.setColour(juce::Colour(0xffaeb9c6));g.drawText(noteColumnName(col),h.reduced(5,0),juce::Justification::centredLeft,true);
        }
        for(int row=0;row<12&&first+row<int(list.size());++row) {
            const int key=list[size_t(first+row)];const auto v=selectedNoteValue(key);
            for(int col=0;col<4;++col) {
                const bool active=key==noteSelected&&col==noteColumn;
                auto r=juce::Rectangle<int>(noteArea.getX()+col*w,noteArea.getY()+25*(row+1),w,25);
                g.setColour(active?juce::Colour(0xff304d66):juce::Colour(0xff272e35));g.fillRect(r);
                g.setColour(active?juce::Colour(0xffb0d6ff):juce::Colour(0xff657481));g.drawRect(r,active?2:1);
                g.setColour(juce::Colours::white);g.drawFittedText(juce::MidiMessage::getMidiNoteName(key,true,true,4)+" "+juce::String(key)+": "+noteValue(v,col),r.reduced(5,1),juce::Justification::centredLeft,1);
            }
        }
    }
    void updateNoteCell(bool announceValue) {
        const auto text=noteSelected<0?juce::String("No assigned notes. Alt+M enables MIDI learn."):
            noteName(noteSelected)+", "+noteColumnName(noteColumn)+", "+noteValue(selectedNoteValue(noteSelected),noteColumn);
        noteCell.setButtonText(text);noteCell.setName(text);noteCell.setTitle(text);repaint();
        if(announceValue)speak(noteCell,text);
    }
    void openNoteGrid() {
        if(chooser||catalogBusy||importInProgress||cached.loading)return;
        noteChannel=selectedChannel();noteProgram=proc.current(noteChannel);
        if(!findPreset(cached.presets,noteProgram)) {announce("Select a mapped program first");return;}
        noteRows=proc.notesForProgram(noteProgram);noteSelected=noteRows.empty()?-1:noteRows.begin()->first;noteColumn=0;noteLearn=false;
        hideMainForPanel();hidePanelControls();panelMode=Panel::notes;
        const auto* preset=findPreset(cached.presets,noteProgram);
        setPanelLabel(juce::String(preset&&preset->sfzIndex<0&&sf2MultiOutput?"SF2 note mixer. ":"Note mixer: audio not yet supported for this engine. ")+"Bank "+number(noteProgram>>7)+", Program "+number(noteProgram&127)+", Channel "+juce::String(noteChannel+1)+". MIDI learn off. Alt+M toggles learn. Alt+arrows edit.");
        noteCell.setVisible(true);panelLabel.setVisible(true);panelCancel.setButtonText("Close");panelCancel.setTitle("Close");panelCancel.setVisible(true);
        noteEventSeen=proc.lastLearnedNote();proc.setNoteLearnChannel(-1);startTimerHz(30);
        resized();updateNoteCell(false);focus(noteCell);announce(panelLabel.getText());
    }
    void pollNoteLearn() {
        const auto event=proc.lastLearnedNote();if(event==noteEventSeen)return;noteEventSeen=event;
        if(!noteLearn||!hasKeyboardFocus(true)||int((event&0xffffffffull)>>7)!=noteProgram)return;
        const int next=int(event&127u);if(next==noteSelected&&noteColumn==0)return;
        noteSelected=next;noteColumn=0;focus(noteCell);updateNoteCell(true);
    }
    void changeNoteValue(int direction,bool reset=false,bool coarse=false,bool boundary=false) {
        if(noteSelected<0)return;
        auto value=selectedNoteValue(noteSelected);const auto before=value;const lsfx::NoteAssignment defaults;
        const int delta=direction*stepWidths[stepIndex]*(coarse?pageMultiplier:1);
        auto adjust=[&](int old,int low,int high,int amount) {return boundary?(direction>0?high:low):juce::jlimit(low,high,old+amount);};
        switch(noteColumn) {
            case 0:value.output=reset?defaults.output:adjust(value.output,-1,stereoBuses-1,direction);break;
            case 1:value.level=reset?0:adjust(value.level,-600,240,delta);break;
            case 2:value.pan=reset?0:adjust(value.pan,-100,100,delta);break;
            default:value.pitch=reset?0:adjust(value.pitch,-1200,1200,delta);break;
        }
        if(value.output==before.output&&value.level==before.level&&value.pan==before.pan&&value.pitch==before.pitch)return;
        proc.setNote(noteProgram,noteSelected,value);noteRows=proc.notesForProgram(noteProgram);updateNoteCell(false);speak(noteCell,noteValue(value,noteColumn));
    }
    bool handleNoteKey(const juce::KeyPress& k) {
        const auto key=k.getKeyCode();const auto m=k.getModifiers();const bool alt=m.isAltDown(),ctrl=m.isCtrlDown()||m.isCommandDown();
        const bool enter=key==juce::KeyPress::returnKey;
        if(key==juce::KeyPress::escapeKey||(alt&&enter)) {closePanel();return true;}
        if(alt&&!ctrl&&juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key))=='m') {
            noteLearn=!noteLearn;noteEventSeen=proc.lastLearnedNote();proc.setNoteLearnChannel(noteLearn?noteChannel:-1);
            announce(noteLearn?"MIDI learn on":"MIDI learn off");return true;
        }
        if(key==juce::KeyPress::tabKey) {focus(panelCancel.hasKeyboardFocus(true)?static_cast<juce::Component&>(noteCell):static_cast<juce::Component&>(panelCancel));return true;}
        if(panelCancel.hasKeyboardFocus(true)) {if(enter||key==juce::KeyPress::spaceKey)closePanel();return true;}
        if(enter||key==juce::KeyPress::spaceKey) {if(noteSelected>=0)updateNoteCell(true);return true;}
        if(key==juce::KeyPress::backspaceKey&&!ctrl) {changeNoteValue(0,true);return true;}
        if(key==juce::KeyPress::deleteKey&&noteSelected>=0) {
            const int old=noteSelected;proc.setNote(noteProgram,old,{});noteRows=proc.notesForProgram(noteProgram);
            auto next=noteRows.lower_bound(old);if(next==noteRows.end()&&!noteRows.empty())next=std::prev(noteRows.end());
            noteSelected=next==noteRows.end()?-1:next->first;updateNoteCell(true);return true;
        }
        const bool horizontal=key==juce::KeyPress::leftKey||key==juce::KeyPress::rightKey;
        const bool vertical=key==juce::KeyPress::upKey||key==juce::KeyPress::downKey;
        const bool page=key==juce::KeyPress::pageUpKey||key==juce::KeyPress::pageDownKey;
        const bool boundary=key==juce::KeyPress::homeKey||key==juce::KeyPress::endKey;
        const int direction=key==juce::KeyPress::upKey||key==juce::KeyPress::pageUpKey||key==juce::KeyPress::homeKey||key==juce::KeyPress::rightKey?1:-1;
        // Same direct Alt gestures as Alt+L. Plain arrows always navigate rows/columns.
        if(alt&&!ctrl&&!m.isShiftDown()) {
            if(horizontal) {changeStep(noteCell,direction);return true;}
            if(vertical||page||boundary) {changeNoteValue(direction,false,page,boundary);return true;}
        }
        if(alt|| (ctrl&&!boundary)) return true;
        if(horizontal) {
            const int next=juce::jlimit(0,3,noteColumn+direction);
            if(next!=noteColumn) {noteColumn=next;updateNoteCell(true);}return true;
        }
        if(boundary||vertical||page) {
            const auto list=visibleNoteRows();if(list.empty())return true;
            int index=int(std::find(list.begin(),list.end(),noteSelected)-list.begin());
            const int oldColumn=noteColumn;
            if(key==juce::KeyPress::homeKey) {index=0;if(ctrl)noteColumn=0;}
            else if(key==juce::KeyPress::endKey) {index=int(list.size())-1;if(ctrl)noteColumn=3;}
            else index+=key==juce::KeyPress::upKey?-1:key==juce::KeyPress::downKey?1:key==juce::KeyPress::pageUpKey?-12:12;
            const int next=list[size_t(juce::jlimit(0,int(list.size())-1,index))];
            if(next!=noteSelected||noteColumn!=oldColumn) {noteSelected=next;updateNoteCell(true);}return true;
        }
        return true;
    }
    void requestClearLocation(bool entireBank) {
        const int address=proc.current(selectedChannel());
        const int b=bank.getSelectedId()>0?bank.getSelectedId()-1:address>>7;
        const int p=entireBank?-1:program.getSelectedId()>0?program.getSelectedId()-1:address&127;
        auto* target=entireBank?static_cast<juce::Component*>(&bank):static_cast<juce::Component*>(&program);
        if(std::none_of(cached.presets.begin(),cached.presets.end(),[&](const Preset& item){return item.bank==b&&(p<0||p==item.program);})) {speak(*target,"Empty");return;}
        const auto mapRevision=proc.getRevision();
        const auto name=p<0?"Bank "+number(b):"Bank "+number(b)+", Program "+number(p);
        showConfirmation("Clear "+name+" from the virtual map? Source files will not be deleted.","Clear",[this,b,p,target,mapRevision]{
            closePanel(target);
            if(proc.getRevision()!=mapRevision) {announce("Map changed; clear cancelled");return;}
            const auto result=proc.clearLocation(b,p);
            if(result.failed()) announce(result.getErrorMessage());else {announceLoaded=true;refresh(true);}
        });
    }
    void requestInitialize(juce::Component* origin) {
        juce::Component::SafePointer<juce::Component> previous(origin);
        const bool wasGrid=gridOpen;
        showConfirmation("Initialize LSFX? All instruments and unsaved changes will be cleared.","Initialize",[this]{closePanel(&open);initializeInstance();},[this,previous,wasGrid]{
            closePanel();
            if(wasGrid&&!gridOpen) {rebuildGrid();showGrid(true);}
            if(auto* c=previous.getComponent();c&&c->isShowing()&&c->isEnabled()) focus(*c);
            else if(wasGrid) focus(grid);
        });
    }
    void initializeInstance() {
        if(chooser) return;
        pendingBrowserPreset=juce::File{};
        announceLoaded=false; initialFocusPending=false; returnFromChooser=false;
        stepIndex=0; lastGainChannel=0; lastRouteChannel=0;
        proc.editorMemory=Processor::EditorMemory{};
        lastSf2Row=rowGain; lastSfzRow=rowGain; shownCode=-1;
        search.reset();
        proc.initializeInstance();
        if(gridOpen) showGrid(false,&open);
        refresh(true);
        focus(open);
        announce("Initialized. All instruments cleared.");
    }
    void showStatus() {
        const auto text=proc.getView().status+". "+proc.diagnostic();
        if(auto* current=juce::Component::getCurrentlyFocusedComponent(); current!=nullptr&&isParentOf(current)) speak(*current,text);
        // No speech if there is no focused accessible control.
    }
    static lsfx::sf2export::Path exportPath(const juce::File& f) {return std::filesystem::u8path(f.getFullPathName().toRawUTF8());}
    void exportMessage(const juce::String& text,int from) {
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::exportProgress; panelReturn=from;
        setPanelLabel(text); panelCancel.setButtonText("OK"); panelCancel.setTitle("OK"); panelCancel.setDescription(text);
        panelLabel.setVisible(true); panelCancel.setVisible(true); resized(); repaint(); focus(panelCancel); announce(text);
        juce::Component::SafePointer<Editor> safe(this);
        juce::Timer::callAfterDelay(150,[safe]{if(auto* e=safe.getComponent();e&&e->panelMode==Panel::exportProgress&&e->isShowing()&&e->getPeer()&&e->getPeer()->isFocused()&&!e->panelCancel.hasKeyboardFocus(true)) {e->focus(e->panelCancel);e->announce(e->panelLabel.getText());}});
    }
    void browseExport() {
        if(chooser||catalogBusy||importInProgress||exportJob||panelMode!=Panel::none) return;
        const int id=focusId(juce::Component::getCurrentlyFocusedComponent()); const int from=id?id:proc.editorMemory.focus;
        const auto snapshot=proc.getView();
        if(snapshot.loading) {exportMessage("Wait until instrument loading has completed before exporting.",from);return;}
        if(!snapshot.ready||snapshot.presets.empty()) {exportMessage("The map is empty. Nothing to export.",from);return;}
        std::vector<lsfx::sf2export::Entry> entries;
        for(const auto& p:snapshot.presets) {
            if(!juce::File(p.path).hasFileExtension("sf2;sfz")) {exportMessage("Export supports PCM16 SF2 and sampled SFZ sources only. Nothing was exported.",from);return;}
            if(p.bank>128) {exportMessage("A mapped bank is outside the standard SF2 range 0 to 128. Nothing was exported.",from);return;}
            entries.push_back({exportPath(juce::File(p.path)),p.sourceBank,p.sourceProgram,p.bank,p.program});
        }
        rememberFocus(from); initialFocusPending=false; returnFromChooser=false; pendingReturnMessage.clear();
        auto dir=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        const auto saved=setting("exportDirectory"); if(juce::File::isAbsolutePath(saved)&&juce::File(saved).isDirectory()) dir=juce::File(saved);
        chooser=std::make_unique<juce::FileChooser>("Export SoundFont",dir.getChildFile("My SoundFont.sf2"),"*.sf2",true,false,this);
        juce::Component::SafePointer<Editor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting,[safe,from,entries=std::move(entries)](const juce::FileChooser& fc) mutable {
            auto file=fc.getResult();
            juce::MessageManager::callAsync([safe,from,file,entries=std::move(entries)]() mutable {if(auto* e=safe.getComponent()) {
                e->chooser.reset();
                if(file==juce::File{}) {e->returnTo(from);return;}
                if(!file.hasFileExtension("sf2")) {e->exportMessage("Please choose a filename ending in .sf2. Nothing was exported.",from);return;}
                e->startExport(file,std::move(entries),from);
            }});
        });
    }
    void startExport(juce::File file,std::vector<lsfx::sf2export::Entry> entries,int from) {
        const auto destination=exportPath(file);
        for(const auto& p:entries) {std::error_code ec;if(destination==p.path||std::filesystem::equivalent(destination,p.path,ec)) {exportMessage("Cannot overwrite a source SoundFont. Choose a new filename.",from);return;}}
        rememberSetting("exportDirectory",file.getParentDirectory().getFullPathName());
        if(exportThread.joinable()) exportThread.join();
        auto job=std::make_shared<ExportJob>(); exportJob=job;exportProgressShown=-1;
        hideMainForPanel(); hidePanelControls();panelMode=Panel::exportProgress;panelReturn=from;
        setPanelLabel("Exporting SoundFont, 0 percent"); panelCancel.setButtonText("Cancel"); panelCancel.setTitle("Cancel"); panelCancel.setDescription("Cancels SoundFont export");
        panelLabel.setVisible(true);panelCancel.setVisible(true);resized();repaint();focus(panelCancel);announce(panelLabel.getText());
        try {exportThread=std::thread([job,file,entries=std::move(entries)]() mutable {
            try {
                juce::TemporaryFile temp(file);
                // Convert each distinct SFZ only once, then use the validated selective SF2 assembler.
                juce::AudioFormatManager formats; formats.registerBasicFormats();
                lsfx::sfzexport::Decode decode=[&](const lsfx::sf2export::Path& path) {return decodeSfzExportSample(formats,path,&job->cancel);};
                std::map<lsfx::sf2export::Path,std::unique_ptr<juce::TemporaryFile>> converted;
                size_t convertedRegions=0,skippedRegions=0;
                std::set<std::string> warnings;
                for(auto& entry:entries) {
                    lsfx::sf2export::checkCancel(&job->cancel);
                    if(lsfx::sfzexport::lower(entry.path.extension().u8string())!=".sfz") continue;
                    auto it=converted.find(entry.path);
                    if(it==converted.end()) {
                        auto intermediate=std::make_unique<juce::TemporaryFile>(juce::String(".sf2"));
                        lsfx::sfzexport::Report report;
                        try {lsfx::sfzexport::convert(entry.path,exportPath(intermediate->getFile()),decode,report,&job->cancel);}
                        catch(const std::exception& error) {throw std::runtime_error(entry.path.u8string()+": "+error.what());}
                        convertedRegions+=report.regions; skippedRegions+=report.skipped;
                        warnings.insert(report.warnings.begin(),report.warnings.end());
                        it=converted.emplace(entry.path,std::move(intermediate)).first;
                    }
                    entry.fromSfz=true; entry.path=exportPath(it->second->getFile()); entry.sourceBank=0; entry.sourceProgram=0;
                }
                auto result=lsfx::sf2export::exportFile(std::move(entries),exportPath(temp.getFile()),&job->cancel,&job->progress);
                lsfx::sf2export::checkCancel(&job->cancel);
                if(!temp.overwriteTargetFileWithTemporary()) throw std::runtime_error("Unable to replace destination file");
                job->message="SoundFont export completed. Presets: "+juce::String(static_cast<juce::int64>(result.presets))+", instruments: "+juce::String(static_cast<juce::int64>(result.instruments))+", samples: "+juce::String(static_cast<juce::int64>(result.samples))+", sources: "+juce::String(static_cast<juce::int64>(result.sources))+", size: "+juce::String(double(result.bytes)/1000000.0,2)+" MB.";
                if(!converted.empty()) {
                    job->message+=" SFZ sources: "+juce::String(static_cast<juce::int64>(converted.size()))+", converted regions: "+juce::String(static_cast<juce::int64>(convertedRegions))+", omitted regions: "+juce::String(static_cast<juce::int64>(skippedRegions))+". SFZ conversion is approximate.";
                    if(!warnings.empty()) {job->message+=" Conversion warnings: ";for(const auto& warning:warnings)job->message+=juce::String::fromUTF8(warning.c_str())+"; ";}
                }
            } catch(const std::exception& e) {job->message=job->cancel.load()?juce::String("SoundFont export cancelled."):juce::String("SoundFont export failed: ")+juce::String::fromUTF8(e.what());}
            catch(...) {job->message="SoundFont export failed.";}
            job->done.store(true);
        });} catch(const std::exception&) {job->message="Cannot start SoundFont export.";job->done.store(true);}
        // Native dialog release may lag behind the JUCE callback.
        juce::Component::SafePointer<Editor> safe(this);
        juce::Timer::callAfterDelay(150,[safe]{if(auto* e=safe.getComponent();e&&e->panelMode==Panel::exportProgress&&e->isShowing()&&e->getPeer()&&e->getPeer()->isFocused()) {e->focus(e->panelCancel);e->announce(e->panelLabel.getText());}});
    }
    void pollExport() {
        if(!exportJob)return;
        if(exportJob->done.load()) {
            if(exportThread.joinable())exportThread.join();
            auto text=exportJob->message;exportJob.reset();
            if(panelMode==Panel::exportProgress) {setPanelLabel(text);panelCancel.setButtonText("OK");panelCancel.setTitle("OK");panelCancel.setDescription(text);if(hasKeyboardFocus(true)) {focus(panelCancel);announce(text);}}
        } else if(!exportJob->cancel.load()) {
            int p=exportJob->progress.load();if(p/10!=exportProgressShown/10||exportProgressShown<0) {exportProgressShown=p;setPanelLabel("Exporting SoundFont, "+juce::String(p)+" percent");}
        }
    }
    void browse() {
        if(chooser||catalogBusy||importInProgress||panelMode!=Panel::none) return;
        const int from=focusId(juce::Component::getCurrentlyFocusedComponent());
        const int returnId=from?from:proc.editorMemory.focus;
        rememberFocus(returnId);
        initialFocusPending=false; returnFromChooser=false; announceLoaded=false; pendingReturnMessage.clear();
        auto start=cached.path.isNotEmpty()?juce::File(cached.path).getParentDirectory():juce::File{};
        const auto previous=setting("instrumentPath");
        if(!start.isDirectory()&&juce::File::isAbsolutePath(previous)) start=juce::File(previous).getParentDirectory();
        if(!start.isDirectory()) start=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser=std::make_unique<juce::FileChooser>("Open SF2 or SFZ",start,"*.sf2;*.sfz",true,false,this);
        juce::Component::SafePointer<Editor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe,returnId](const juce::FileChooser& fc) {
            if(auto* self=safe.getComponent()) {
                auto file=fc.getResult();
                if(file!=juce::File{}) { rememberSetting("instrumentPath",file.getFullPathName()); self->pendingBrowserPreset=juce::File{}; self->announceLoaded=true; self->proc.loadFile(file); }
                juce::MessageManager::callAsync([safe,returnId]{if(auto* e=safe.getComponent()) {
                    e->chooser.reset(); e->refresh(true); e->returnTo(returnId);
                }});
            }
        });
    }
    static juce::String number(int n) {return juce::String(n).paddedLeft('0',3);}
    void browseImport(bool single) {
        if(chooser||catalogBusy||cached.loading||importInProgress) return;
        importInProgress=true; pendingReturnMessage.clear(); chooserReturn=single?38:37; rememberFocus(chooserReturn);
        const int ch=selectedChannel(),code=proc.current(ch),destBank=bank.getSelectedId()>0?bank.getSelectedId()-1:code>>7,destProgram=program.getSelectedId()>0?program.getSelectedId()-1:code&127;
        auto start=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        const auto previous=setting("instrumentPath");
        if(juce::File::isAbsolutePath(previous)&&juce::File(previous).getParentDirectory().isDirectory()) start=juce::File(previous).getParentDirectory();
        chooser=std::make_unique<juce::FileChooser>(single?"Import SF2 or SFZ program":"Import SF2 bank",start,single?"*.sf2;*.sfz":"*.sf2",true,false,this);
        juce::Component::SafePointer<Editor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe,single,ch,destBank,destProgram](const juce::FileChooser& fc){
            const auto file=fc.getResult();
            juce::MessageManager::callAsync([safe,single,ch,destBank,destProgram,file]{if(auto* e=safe.getComponent()) {
                e->chooser.reset();
                if(file==juce::File{}) {e->endImport();return;}
                rememberSetting("instrumentPath",file.getFullPathName());
                if(single&&file.hasFileExtension("sfz")) {e->finishImport(e->proc.importSFZ(file,pack(destBank,destProgram),ch));return;}
                if(!file.hasFileExtension("sf2;sf3")) {e->endImport("Unsupported file format");return;}
                e->scanImport(file,single,ch,destBank,destProgram);
            }});
        });
    }
    void endImport(const juce::String& message={}) {
        const int destination=chooserReturn;
        if(panelMode==Panel::source) closePanel(&focusTarget(destination));
        importInProgress=false; pendingReturnMessage=message;
        refresh(true); returnTo(destination);
    }
    void finishImport(const juce::Result& result) {
        endImport(result.failed()?result.getErrorMessage():juce::String{});
        if(result.wasOk()) announceLoaded=true;
    }
    void scanImport(juce::File file,bool single,int ch,int destBank,int destProgram) {
        if(catalogThread.joinable()) catalogThread.join();
        catalogBusy=true; announce("Reading SoundFont banks");
        juce::Component::SafePointer<Editor> safe(this);
        const auto startRevision=proc.getRevision();
        catalogThread=std::thread([safe,file,single,ch,destBank,destProgram,startRevision]{
            auto data=std::make_shared<CatalogResult>(inspectSF2(file));
            juce::MessageManager::callAsync([safe,file,single,ch,destBank,destProgram,data,startRevision]{if(auto* e=safe.getComponent()) {
                e->catalogBusy=false; if(e->catalogThread.joinable()) e->catalogThread.join(); e->refresh(true);
                if(e->proc.getRevision()!=startRevision) {e->endImport("Instrument changed; import cancelled");return;}
                if(data->error.isNotEmpty()) {e->endImport(data->error);return;}
                e->chooseSourceBank(file,single,ch,destBank,destProgram,data);
            }});
        });
    }
    void showSourceList(const juce::String& titleText,std::vector<std::pair<int,juce::String>> entries,std::function<void(int)> action) {
        if(entries.empty()) {endImport("No source entries");return;}
        hideMainForPanel(); hidePanelControls(); panelMode=Panel::source;
        sourceEntries=std::move(entries); sourceAction=std::move(action); panelCancelAction=[this]{endImport();};
        setPanelLabel(titleText); presetList.setName(titleText); presetList.setTitle(titleText);
        panelCancel.setButtonText("Cancel"); panelCancel.setTitle("Cancel");
        for(auto* c:std::array<juce::Component*,3>{&presetList,&panelCancel,&panelLabel}) c->setVisible(true);
        presetList.updateContent(); presetList.deselectAllRows(); presetList.selectRow(0); presetList.scrollToEnsureRowIsOnscreen(0);
        resized(); repaint(); focus(presetList); announce(titleText+". "+getNameForRow(0));
        if(!presetList.hasKeyboardFocus(true)) {
            juce::Component::SafePointer<Editor> safe(this);
            juce::Timer::callAfterDelay(150,[safe]{if(auto* e=safe.getComponent();e&&e->panelMode==Panel::source&&e->isShowing()&&e->getPeer()!=nullptr&&e->getPeer()->isFocused()) {auto* c=juce::Component::getCurrentlyFocusedComponent(); if(!c||c==e||e->isParentOf(c)) {e->focus(e->presetList);e->announce(e->panelLabel.getText()+". "+e->getNameForRow(e->presetList.getSelectedRow()));}}});
        }
    }
    void activateSource(int row) {
        if(!juce::isPositiveAndBelow(row,getNumRows())||!sourceAction)return;
        const int id=sourceEntries[static_cast<size_t>(row)].first; auto action=sourceAction; action(id);
    }
    void chooseSourceBank(const juce::File& file,bool single,int ch,int destBank,int destProgram,std::shared_ptr<CatalogResult> data) {
        std::vector<std::pair<int,juce::String>> entries; int last=-1;
        for(const auto& p:data->presets) if(p.bank!=last) {last=p.bank;entries.emplace_back(p.bank,"Bank "+number(p.bank)+", "+sourceName(p.path));}
        const auto startRevision=proc.getRevision();
        showSourceList("Select source bank",std::move(entries),[this,file,single,ch,destBank,destProgram,data,startRevision](int id){
            if(proc.getRevision()!=startRevision) {endImport("Instrument changed; import cancelled");return;}
            if(single) chooseSourceProgram(file,ch,destBank,destProgram,id,data);
            else finishImport(proc.importSF2(file,destBank,-1,id,0,ch));
        });
    }
    void chooseSourceProgram(const juce::File& file,int ch,int destBank,int destProgram,int sourceBank,std::shared_ptr<CatalogResult> data) {
        std::vector<std::pair<int,juce::String>> entries;
        for(const auto& p:data->presets) if(p.bank==sourceBank) entries.emplace_back(p.program,"Program "+number(p.program)+", "+p.name+", "+sourceName(p.path));
        const auto startRevision=proc.getRevision();
        showSourceList("Select source program",std::move(entries),[this,file,ch,destBank,destProgram,sourceBank,startRevision](int id){
            if(proc.getRevision()!=startRevision) {endImport("Instrument changed; import cancelled");return;}
            finishImport(proc.importSF2(file,destBank,destProgram,sourceBank,id,ch));
        });
    }
    void rebuildPrograms(int b,int p) {
        program.clear(juce::dontSendNotification);
        for(int i=0;i<lsfx::programCount;++i) {
            const auto* item=findPreset(cached.presets,pack(b,i));
            program.addItem("Program "+number(i)+", "+(item?item->name+", "+sourceName(item->path):juce::String("Empty")),i+1);
        }
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
        if(changed) {cached=proc.getView();revision=cached.revision;status.setText(cached.status,juce::dontSendNotification);status.setDescription(cached.status); if(cached.ready&&!cached.loading&&cached.path.isNotEmpty()&&cached.path!=lastRememberedInstrument) {lastRememberedInstrument=cached.path;rememberSetting("instrumentPath",cached.path);} }
        if(pendingBrowserPreset!=juce::File{}&&!cached.loading&&proc.currentPresetFile()==pendingBrowserPreset) {
            if(cached.ready||cached.path.isEmpty()) {preset_browser_memory::remember(presetRoot(),pendingBrowserPreset,settingsName);pendingBrowserPreset=juce::File{};}
        }
        if(channel.getSelectedId()!=proc.uiChannel()+1&&!channel.isPopupActive()) {channel.setSelectedId(proc.uiChannel()+1,juce::dontSendNotification);shownCode=-1;}
        const int code=proc.current(selectedChannel()); const int previousCode=shownCode;
        if(force||changed||code!=shownCode) {
            if(!force&&!changed&&(bank.isPopupActive()||program.isPopupActive())) return;
            updating=true;
            if(changed||force||(code>>7)>lsfx::percussionBank) {
                bank.clear(juce::dontSendNotification);
                std::map<int,juce::StringArray> names;
                for(int i=0;i<=lsfx::percussionBank;++i) names[i];
                for(const auto& p:cached.presets) names[p.bank].addIfNotAlreadyThere(sourceName(p.path));
                names[code>>7]; // retain existing extended MIDI bank addresses
                for(const auto& item:names) bank.addItem("Bank "+number(item.first)+", "+(item.second.isEmpty()?juce::String("Empty"):item.second.joinIntoString(", ")),item.first+1);
            }
            bank.setSelectedId((code>>7)+1,juce::dontSendNotification); rebuildPrograms(code>>7,code&127); shownCode=code;
            updating=false;
        }
        routingLabel.setText(!sf2MultiOutput?"Stereo output routing - SFZ active, SF2 Main 1/2 only - Alt+R":"Stereo output routing - Alt+R",juce::dontSendNotification);
        if(changed||force||code!=previousCode) for(auto& c:routeLevels) c.updateText();
        // Keep the focused native controls enabled during loading. Their edit handlers guard pending requests.
        if(!dragging) { const auto gain=proc.gainParameter().get(); if(std::abs(level.getValue()-gain)>0.000001) level.setValue(gain,juce::dontSendNotification); const auto text=juce::String(gain,1)+" dB"; if(levelReadout.getText()!=text) levelReadout.setText(text,juce::dontSendNotification); }
        for(int ch=0;ch<channels;++ch) {
            auto& c=channelLevels[ch];
            const bool enabled=true;
            if(c.isEnabled()!=enabled) c.setEnabled(enabled);
            auto& routing=routeLevels[ch]; if(routing.isEnabled()!=enabled) routing.setEnabled(enabled);
            if(!routeDragging[ch]&&routing.getValue()!=proc.routeParameter(ch).getIndex()) {routing.setValue(proc.routeParameter(ch).getIndex(),juce::dontSendNotification);repaint();}
            if(!channelDragging[ch]) {const auto v=proc.channelGainParameter(ch).get(); if(std::abs(c.getValue()-v)>0.000001) {c.setValue(v,juce::dontSendNotification);repaint();}}
        }
        if(gridOpen) {if(changed||code!=gridShownCode) {gridShownCode=code;rebuildGrid();} else refreshGridValues();}
        if(announceLoaded&&!cached.loading&&!chooser&&!returnFromChooser&&!importInProgress&&hasKeyboardFocus(true)) {
            announceLoaded=false;
            if(bank.hasKeyboardFocus(true)&&(cached.status.startsWith("Loaded:")||cached.path.isEmpty())) publishReturnedFocus(bank);
            else if(program.hasKeyboardFocus(true)&&(cached.status.startsWith("Loaded:")||cached.path.isEmpty())) publishReturnedFocus(program);
            else if(auto* c=juce::Component::getCurrentlyFocusedComponent();c&&isParentOf(c)) speak(*c,cached.status);
        }
    }
    void timerCallback() override {
        if(panelMode==Panel::instrument) {refresh(false);return;}
        if(panelMode==Panel::notes) {
            if(proc.getRevision()!=revision||proc.current(noteChannel)!=noteProgram) {closePanel();refresh(true);return;}
            pollNoteLearn();return;
        }
        pollExport();
        if(returnFromChooser&&juce::Time::getMillisecondCounter()-chooserReturnedAt>5000u) returnFromChooser=false;
        refresh(false); maybePromptMissing();
        if(panelMode==Panel::none&&(initialFocusPending||returnFromChooser)&&isShowing()&&getPeer()!=nullptr&&(returnFromChooser||getPeer()->isFocused())) scheduleInitialFocus();
    }
};
juce::AudioProcessorEditor* Processor::createEditor(){return new Editor(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Processor();}
