// Exercise the real processor, not merely the independent native APIs.
#include "../Source/Plugin.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
juce::File absolute(const char* path) {
    return juce::File::isAbsolutePath(path)?juce::File(path):juce::File::getCurrentWorkingDirectory().getChildFile(path);
}
void waitReady(Processor& p,bool expectedReady=true) {
    for(int i=0;i<1500;++i) {
        const auto v=p.getView();
        if(!v.loading) {
            std::cout<<v.status.toStdString()<<std::endl;
            require(v.ready==expectedReady,"unexpected loader result"); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("instrument loader timed out");
}
void setChoice(juce::AudioParameterChoice& p,int value) {
    p.setValueNotifyingHost(p.convertTo0to1(static_cast<float>(value)));
}
std::array<float,3> render(Processor& p,juce::MidiBuffer& midi,int blocks=24) {
    juce::AudioBuffer<float> b(p.getTotalNumOutputChannels(),256);
    require(b.getNumChannels()>=6,"output bus layout");
    std::array<float,3> peak{};
    for(int block=0;block<blocks;++block) {
        p.processBlock(b,midi);
        midi.clear();
        for(int ch=0;ch<b.getNumChannels();++ch) for(int n=0;n<b.getNumSamples();++n)
            require(std::isfinite(b.getSample(ch,n)),"nonfinite sample");
        for(int pair=0;pair<3;++pair) peak[pair]=(std::max)(peak[pair],(std::max)(b.getMagnitude(2*pair,0,256),b.getMagnitude(2*pair+1,0,256)));
    }
    return peak;
}
}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    try {
        juce::ScopedJuceInitialiser_GUI juceInit;
        const auto sf2=absolute(argv[1]), one=absolute(argv[2]), two=absolute(argv[3]);
        Processor p; p.prepareToPlay(44100,256);
        p.loadFile(sf2); waitReady(p);
        require(p.importSFZ(one,pack(0,3),1).wasOk(),"SFZ import one"); waitReady(p);
        require(p.importSFZ(two,pack(0,4),2).wasOk(),"SFZ import two"); waitReady(p);
        const auto v=p.getView();
        require(findPreset(v.presets,pack(0,0))!=nullptr,"original SF2 retained");
        require(findPreset(v.presets,pack(0,3))&&findPreset(v.presets,pack(0,3))->path==one.getFullPathName(),"first SFZ mapped");
        require(findPreset(v.presets,pack(0,4))&&findPreset(v.presets,pack(0,4))->path==two.getFullPathName(),"second SFZ mapped");
        setChoice(p.effectsParameter(),0); setChoice(p.midiFXParameter(),0);
        setChoice(p.routeParameter(1),1); setChoice(p.routeParameter(2),2);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),17);
        midi.addEvent(juce::MidiMessage::noteOn(3,60,juce::uint8(100)),103);
        auto peak=render(p,midi);
        std::cout<<"Hybrid peaks: "<<peak[0]<<", "<<peak[1]<<", "<<peak[2]<<std::endl;
        require(peak[0]>1e-5f&&peak[1]>1e-5f&&peak[2]>1e-5f,"three independent outputs");
        // Clear all notes, then ensure an SFZ alone is never duplicated into Main.
        p.allOff(); render(p,midi,12);
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),11);
        peak=render(p,midi);
        require(peak[1]>1e-5f&&peak[0]<1e-5f&&peak[2]<1e-5f,"SFZ only on its separate output");
        p.allOff(); render(p,midi,12);
        // Select a second instance of the same SFZ. The worker must prepare the
        // extra slot without loading on the audio thread or touching the first.
        p.select(1,0,4); render(p,midi,1);
        bool duplicateReady=false;
        for(int tries=0;tries<100&&!duplicateReady;++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),0);
            peak=render(p,midi,2); duplicateReady=peak[1]>1e-5f;
            midi.addEvent(juce::MidiMessage::noteOff(2,60),0); render(p,midi,1);
        }
        require(duplicateReady,"second channel using same SFZ");
        p.allOff(); render(p,midi,12);
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),0);
        midi.addEvent(juce::MidiMessage::noteOn(3,60,juce::uint8(100)),0);
        peak=render(p,midi);
        require(peak[1]>1e-5f&&peak[2]>1e-5f&&peak[0]<1e-5f,"same source independent channels");
        juce::MemoryBlock state; p.getStateInformation(state);
        Processor restored; restored.prepareToPlay(44100,256); restored.setStateInformation(state.getData(),int(state.getSize())); waitReady(restored);
        const auto restoredView=restored.getView();
        require(findPreset(restoredView.presets,pack(0,3))&&findPreset(restoredView.presets,pack(0,4)),"SFZ map persisted");
        require(restored.current(1)==pack(0,4)&&restored.current(2)==pack(0,4),"channel addresses persisted");
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),0);
        midi.addEvent(juce::MidiMessage::noteOn(3,60,juce::uint8(100)),0);
        peak=render(restored,midi);
        require(peak[1]>1e-5f&&peak[2]>1e-5f&&peak[0]<1e-5f,"restored hybrid playback");
        restored.initializeInstance(); waitReady(restored,false); require(restored.getView().presets.empty(),"initialize clears map");
        std::cout<<"PASS: SF2 + two SFZs, separate routing, duplicate source, state restore, initialize\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
