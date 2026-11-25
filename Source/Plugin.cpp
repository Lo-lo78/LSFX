#include <JuceHeader.h>
#include <fluidsynth.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr int channels = 16;
constexpr int pack(int bank, int program) { return (bank << 7) | program; }
struct Preset { int bank, program; juce::String name; };
struct Engine {
    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    int font = -1;
    double rate = 44100;
    ~Engine() { if (synth) delete_fluid_synth(synth); if (settings) delete_fluid_settings(settings); }
};
using Programs = std::array<int, channels>;
Programs defaults() { Programs p{}; p[9] = pack(128, 0); return p; }
struct Request { juce::String path; double rate = 44100; Programs programs = defaults(); uint64_t serial = 0; };
struct View { juce::String path, status = "No SoundFont loaded"; std::vector<Preset> presets; uint64_t revision = 0; bool loading = false; };
}

class Processor final : public juce::AudioProcessor {
public:
    Processor() : AudioProcessor(BusesProperties().withOutput("Stereo", juce::AudioChannelSet::stereo(), true)) {
        auto p = defaults();
        for (int i=0; i<channels; ++i) { selected[i].store(p[i]); commands[i].store(-1); }
        addParameter(gain = new juce::AudioParameterFloat({"gain",1}, "Output gain", juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), -6.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
        worker = std::thread([this]{ work(); });
    }
    ~Processor() override {
        { std::lock_guard<std::mutex> lock(requestMutex); stopping = true; }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }
    const juce::String getName() const override { return "LSoundFont"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }
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
            juce::String path; Programs p;
            stateForSave(path,p);
            if (path.isNotEmpty()) load(path,p);
        }
    }
    void releaseResources() override { panic.store(true); }
    void reset() override { panic.store(true); }
    View getView() { std::lock_guard<std::mutex> lock(viewMutex); return view; }
    int current(int ch) const { const auto cmd=commands[ch].load(); return cmd>=0 ? cmd : selected[ch].load(); }
    void select(int ch,int bank,int prog) { if (ch>=0 && ch<channels && bank>=0 && bank<=16383 && prog>=0 && prog<128) commands[ch].store(pack(bank,prog)); }
    void allOff() { panic.store(true); }
    void loadFile(const juce::File& f) { load(f.getFullPathName(), defaults()); }
    juce::AudioParameterFloat& gainParameter() { return *gain; }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        b.clear();
        // No waiting for disk I/O or for the worker on the audio thread.
        std::unique_lock<std::mutex> lock(engineMutex, std::try_to_lock);
        if (!lock.owns_lock() || !engine || std::abs(engine->rate-hostRate.load())>0.01 || b.getNumChannels()<2) { midi.clear(); return; }
        auto* s = engine->synth;
        if (panic.exchange(false)) for (int ch=0;ch<channels;++ch) fluid_synth_all_sounds_off(s,ch);
        for (int ch=0; ch<channels; ++ch) {
            const int cmd = commands[ch].exchange(-1);
            if (cmd>=0 && fluid_synth_program_select(s,ch,engine->font,cmd>>7,cmd&127)==FLUID_OK) selected[ch].store(cmd);
        }
        const float g = juce::Decibels::decibelsToGain(gain->get());
        // FluidSynth renders each section preceding the corresponding MIDI event.
        // Its internal block size may still quantise event timing slightly.
        auto render = [&](int start,int count) {
            if (count>0) fluid_synth_write_float(s,count,b.getWritePointer(0,start),0,1,b.getWritePointer(1,start),0,1);
        };
        int cursor=0;
        for (const auto meta : midi) {
            const int pos=juce::jlimit(cursor,b.getNumSamples(),meta.samplePosition);
            render(cursor,pos-cursor); cursor=pos;
            // Avoid allocating a MidiMessage for long SysEx messages.
            const auto* d=meta.data; const int n=meta.numBytes;
            if(n<=0) continue;
            const int status=d[0], ch=status&15;
            if(status==0xf0 && n>=2) {
                int len=n-1; if(d[n-1]==0xf7) --len;
                if(len>0) fluid_synth_sysex(s,reinterpret_cast<const char*>(d+1),len,nullptr,nullptr,nullptr,0);
                for(int c=0;c<channels;++c) updateProgram(s,c);
            } else if(status<0xf0 && status>=0x80 && n>=2) {
                const int a=d[1]&127, v=n>=3 ? d[2]&127 : 0;
                switch(status&0xf0) {
                    case 0x80: if(n>=3) fluid_synth_noteoff(s,ch,a); break;
                    case 0x90: if(n>=3) { if(v) fluid_synth_noteon(s,ch,a,v); else fluid_synth_noteoff(s,ch,a); } break;
                    case 0xa0: if(n>=3) fluid_synth_key_pressure(s,ch,a,v); break;
                    case 0xb0: if(n>=3) fluid_synth_cc(s,ch,a,v); break;
                    case 0xc0: fluid_synth_program_change(s,ch,a); updateProgram(s,ch); break;
                    case 0xd0: fluid_synth_channel_pressure(s,ch,a); break;
                    case 0xe0: if(n>=3) fluid_synth_pitch_bend(s,ch,a|(v<<7)); break;
                    default: break;
                }
            }
        }
        render(cursor,b.getNumSamples()-cursor);
        b.applyGain(g); midi.clear();
    }
    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::String path; Programs p; stateForSave(path,p);
        juce::XmlElement x("LSoundFontState");
        x.setAttribute("version",1); x.setAttribute("file",path); x.setAttribute("gain",static_cast<double>(gain->get()));
        for(int i=0;i<channels;++i) x.setAttribute("ch"+juce::String(i+1),p[i]);
        copyXmlToBinary(x,dest);
    }
    void setStateInformation(const void* data,int size) override {
        if(auto x=getXmlFromBinary(data,size)) if(x->hasTagName("LSoundFontState")) {
            auto p=defaults();
            for(int i=0;i<channels;++i) p[i]=juce::jlimit(0,pack(16383,127),x->getIntAttribute("ch"+juce::String(i+1),p[i]));
            gain->setValueNotifyingHost(gain->convertTo0to1(static_cast<float>(x->getDoubleAttribute("gain",-6.0))));
            load(x->getStringAttribute("file"),p);
        }
    }
private:
    juce::AudioParameterFloat* gain = nullptr;
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
    void stateForSave(juce::String& path, Programs& p) {
        std::lock_guard<std::mutex> lock(requestMutex);
        path=latest.path;
        // While a new bank is loading retain the requested state, not old programs.
        if (pending || loadingSerial!=0 || failedSerial==latest.serial) p=latest.programs;
        else for(int i=0;i<channels;++i) { int cmd=commands[i].load(); p[i]=cmd>=0 ? cmd : selected[i].load(); }
    }
    uint64_t loadingSerial=0, failedSerial=~uint64_t(0);
    void load(const juce::String& path,Programs p) {
        { std::lock_guard<std::mutex> lock(requestMutex);
          latest={path,hostRate.load(),p,latest.serial+1}; pending=true;
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
            juce::String error;
            if(r.path.isNotEmpty()) {
                if(!juce::File(r.path).existsAsFile()) error="SoundFont not found: "+r.path;
                else {
                    fresh=std::make_unique<Engine>(); fresh->rate=r.rate;
                    fresh->settings=new_fluid_settings();
                    if(fresh->settings) {
                        fluid_settings_setnum(fresh->settings,"synth.sample-rate",r.rate);
                        fluid_settings_setint(fresh->settings,"synth.midi-channels",16);
                        fluid_settings_setint(fresh->settings,"synth.polyphony",256);
                        fluid_settings_setint(fresh->settings,"synth.reverb.active",1);
                        fluid_settings_setint(fresh->settings,"synth.chorus.active",1);
                        fluid_settings_setstr(fresh->settings,"synth.midi-bank-select","gs");
                        fresh->synth=new_fluid_synth(fresh->settings);
                    }
                    if(fresh->synth) fresh->font=fluid_synth_sfload(fresh->synth,r.path.toRawUTF8(),1);
                    if(fresh->font<0) error="Unable to load SoundFont: "+r.path;
                    else {
                        fluid_synth_set_interp_method(fresh->synth,-1,FLUID_INTERP_7THORDER);
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
                for(int i=0;i<channels;++i) { commands[i].store(-1); selected[i].store(r.programs[i]); if(engine) updateProgram(engine->synth,i); }
              }
              loadingSerial=0; failedSerial=error.isNotEmpty()?r.serial:~uint64_t(0);
              { std::lock_guard<std::mutex> ui(viewMutex);
                view.path=r.path; view.presets=error.isEmpty()?std::move(presets):std::vector<Preset>{}; view.loading=false;
                view.status=error.isNotEmpty()?error:r.path.isEmpty()?"No SoundFont loaded":"Loaded: "+juce::File(r.path).getFileName()+"; "+juce::String(static_cast<int>(view.presets.size()))+" presets";
                ++view.revision;
              }
            }
            // Destroy old synth and samples here, outside the audio lock and audio thread.
        }
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

class Editor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit Editor(Processor& p) : AudioProcessorEditor(p), proc(p) {
        setWantsKeyboardFocus(true);
        title.setText("LSoundFont 0.0.1 TEST1",juce::dontSendNotification); title.setFont(juce::FontOptions(21.0f)); addAndMakeVisible(title);
        status.setText("No SoundFont loaded",juce::dontSendNotification); status.setTitle("SoundFont status"); addAndMakeVisible(status);
        setup(open,"Open SoundFont",1); setup(channel,"MIDI channel",2); setup(bank,"Bank",3); setup(program,"Program",4); setup(level,"Output gain",5); setup(stop,"Panic - all sounds off",6); setup(readStatus,"Read status",7);
        channelLabel.setText("MIDI channel",juce::dontSendNotification); bankLabel.setText("Bank",juce::dontSendNotification); programLabel.setText("Program",juce::dontSendNotification); levelLabel.setText("Output gain (dB)",juce::dontSendNotification);
        for(auto* l:{&channelLabel,&bankLabel,&programLabel,&levelLabel}) addAndMakeVisible(*l);
        for(int i=1;i<=16;++i) channel.addItem("Channel "+juce::String(i),i);
        channel.setSelectedId(1,juce::dontSendNotification);
        channel.onChange=[this]{ shownCode=-1; refresh(true); };
        bank.onChange=[this]{ if(!updating) { rebuildPrograms(bank.getSelectedId()-1,-1); if(program.getNumItems()>0) { program.setSelectedItemIndex(0,juce::dontSendNotification); apply(); } } };
        program.onChange=[this]{ if(!updating) apply(); };
        open.onClick=[this]{ browse(); };
        stop.onClick=[this]{proc.allOff();};
        readStatus.onClick=[this]{ juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"LSoundFont status",proc.getView().status); };
        level.setSliderStyle(juce::Slider::LinearHorizontal); level.setTextBoxStyle(juce::Slider::TextBoxRight,false,90,25); level.setRange(-60.0,6.0,0.1); level.setValue(proc.gainParameter().get(),juce::dontSendNotification);
        level.onDragStart=[this]{proc.gainParameter().beginChangeGesture(); dragging=true;};
        level.onDragEnd=[this]{dragging=false;proc.gainParameter().endChangeGesture();};
        level.onValueChange=[this]{ auto& a=proc.gainParameter(); if(!dragging) a.beginChangeGesture(); a.setValueNotifyingHost(a.convertTo0to1(static_cast<float>(level.getValue()))); if(!dragging) a.endChangeGesture(); };
        setSize(620,360); refresh(true); startTimerHz(5);
    }
    ~Editor() override { stopTimer(); chooser.reset(); }
    void paint(juce::Graphics& g) override {g.fillAll(juce::Colour(0xff202429));}
    void resized() override {
        auto r=getLocalBounds().reduced(16); title.setBounds(r.removeFromTop(30)); status.setBounds(r.removeFromTop(30)); open.setBounds(r.removeFromTop(30)); r.removeFromTop(10);
        auto line=[&](juce::Label& l,juce::Component& c){auto a=r.removeFromTop(38);l.setBounds(a.removeFromLeft(150));c.setBounds(a.reduced(0,3));};
        line(channelLabel,channel);line(bankLabel,bank);line(programLabel,program);line(levelLabel,level);r.removeFromTop(6);auto a=r.removeFromTop(32);stop.setBounds(a.removeFromLeft(a.getWidth()/2).reduced(2));readStatus.setBounds(a.reduced(2));
    }
private:
    Processor& proc;
    juce::Label title,status,channelLabel,bankLabel,programLabel,levelLabel;
    juce::TextButton open{"Open SoundFont..."},stop{"Panic - all sounds off"},readStatus{"Read status"};
    juce::ComboBox channel,bank,program;
    juce::Slider level;
    std::unique_ptr<juce::FileChooser> chooser;
    View cached;
    uint64_t revision=~uint64_t(0);
    int shownCode=-1;
    bool updating=false,dragging=false;
    template<class C> void setup(C& c,const juce::String& name,int order) { c.setTitle(name); c.setWantsKeyboardFocus(true); c.setExplicitFocusOrder(order); addAndMakeVisible(c); }
    void browse() {
        if(chooser) return;
        auto start=cached.path.isNotEmpty()?juce::File(cached.path).getParentDirectory():juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser=std::make_unique<juce::FileChooser>("Open SoundFont",start,"*.sf2;*.sf3",true);
        juce::Component::SafePointer<Editor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& fc) {
            if(auto* self=safe.getComponent()) { auto file=fc.getResult(); if(file!=juce::File{}) self->proc.loadFile(file); juce::MessageManager::callAsync([safe]{if(auto* e=safe.getComponent()) e->chooser.reset();}); }
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
            proc.select(juce::jmax(0,channel.getSelectedId()-1),b,p); shownCode=pack(b,p);
        }
    }
    void refresh(bool force) {
        auto v=proc.getView();
        const bool changed=v.revision!=revision;
        if(changed) {revision=v.revision;cached=std::move(v);status.setText(cached.status,juce::dontSendNotification);status.setDescription(cached.status);}
        const int code=proc.current(juce::jmax(0,channel.getSelectedId()-1));
        if(force||changed||code!=shownCode) {
            // Avoid rebuilding a combo while the user is navigating its popup.
            if(!force&&!changed&&(bank.isPopupActive()||program.isPopupActive())) return;
            updating=true;
            if(changed||force) {
                bank.clear(juce::dontSendNotification); int last=-1;
                for(const auto& p:cached.presets) if(p.bank!=last) {bank.addItem("Bank "+juce::String(p.bank),p.bank+1);last=p.bank;}
            }
            bank.setSelectedId((code>>7)+1,juce::dontSendNotification); rebuildPrograms(code>>7,code&127); shownCode=code;
            updating=false;
        }
        const bool ready=!cached.loading&&!cached.presets.empty(); bank.setEnabled(ready);program.setEnabled(ready);
        if(!dragging) level.setValue(proc.gainParameter().get(),juce::dontSendNotification);
    }
    void timerCallback() override {refresh(false);}
};
juce::AudioProcessorEditor* Processor::createEditor(){return new Editor(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Processor();}
