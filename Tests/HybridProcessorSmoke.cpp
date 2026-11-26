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
void setComp(Processor& p,int ch,int index,float value) {
    auto& parameter=p.compressorParameter(ch,index);
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}
// Exercise all physical pairs, including the last two pins 33/34, using
// the same processor/render path as the VST3. No Main leakage with FX off.
void testAllSf2Outputs(const juce::File& source) {
    Processor p; p.prepareToPlay(48000,257);
    p.loadFile(source); waitReady(p);
    setChoice(p.effectsParameter(),0); setChoice(p.midiFXParameter(),0);
    require(p.getTotalNumOutputChannels()==34,"all 17 stereo buses enabled");
    for(int ch=0;ch<16;++ch) setChoice(p.routeParameter(ch),ch+1);
    juce::AudioBuffer<float> b(34,257);
    juce::MidiBuffer midi;
    for(int ch=0;ch<16;++ch) midi.addEvent(juce::MidiMessage::noteOn(ch+1,60,juce::uint8(100)),ch);
    std::array<float,17> peak{};
    for(int block=0;block<24;++block) {
        p.processBlock(b,midi); midi.clear();
        for(int pair=0;pair<17;++pair) {
            for(int lr=0;lr<2;++lr) for(int n=0;n<b.getNumSamples();++n)
                require(std::isfinite(b.getSample(pair*2+lr,n)),"nonfinite SF2 output");
            peak[pair]=(std::max)(peak[pair],(std::max)(b.getMagnitude(2*pair,0,257),b.getMagnitude(2*pair+1,0,257)));
        }
    }
    require(peak[0]<1e-5f,"all separate outputs: Main silent");
    for(int pair=1;pair<17;++pair) require(peak[pair]>1e-5f,"missing SF2 stereo pair");
    // All notes remain active, sum any number of channels onto one bus.
    for(int ch=0;ch<16;++ch) setChoice(p.routeParameter(ch),2);
    peak.fill(0);
    for(int block=0;block<16;++block) {
        p.processBlock(b,midi);
        for(int pair=0;pair<17;++pair) peak[pair]=(std::max)(peak[pair],(std::max)(b.getMagnitude(2*pair,0,257),b.getMagnitude(2*pair+1,0,257)));
    }
    require(peak[2]>1e-5f,"shared SF2 bus missing audio");
    for(int pair=0;pair<17;++pair) if(pair!=2) require(peak[pair]<1e-5f,"duplicate SF2 audio on other bus");
    std::cout<<"PASS: SF2 all 16 auxiliary stereo pairs and shared destination\n";
}
void testChannelCompression(const juce::File& source,bool sfz) {
    Processor p; p.prepareToPlay(44100,256);
    p.loadFile(source); waitReady(p);
    // Two MIDI channels with the same instrument. SFZ needs a second mapped address.
    if(sfz) {require(p.importSFZ(source,pack(0,3),1).wasOk(),"second SFZ address");waitReady(p);}
    setChoice(p.effectsParameter(),0); setChoice(p.midiFXParameter(),0);
    setChoice(p.routeParameter(0),1); setChoice(p.routeParameter(1),2);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),0);
    render(p,midi,24);
    const auto dry=render(p,midi,16);
    require(dry[1]>1e-5f&&dry[2]>1e-5f&&dry[0]<1e-5f,"two dry channels on independent outputs");
    setComp(p,0,lsfx::compThreshold,-60); setComp(p,0,lsfx::compRatio,3);
    setComp(p,0,lsfx::compMakeup,0); setComp(p,0,lsfx::compMix,100);
    render(p,midi,24);
    const auto wet=render(p,midi,16);
    require(wet[1]<dry[1]*.6f&&wet[1]>1e-7f,"channel 1 compression applied before routing");
    require(wet[2]>dry[2]*.95f&&wet[2]<dry[2]*1.05f,"channel 2 unaffected by channel 1 compressor");
    require(wet[0]<1e-5f,"no leakage to Main");
    // Move the compressed channel without retriggering it. Send channel 2
    // to a different enabled bus so its tail cannot contaminate the reading.
    setChoice(p.routeParameter(1),3); setChoice(p.routeParameter(0),2);
    render(p,midi,8); const auto moved=render(p,midi,8);
    require(moved[1]<1e-5f&&moved[0]<1e-5f,"routing moved without duplicate");
    require(moved[2]>wet[1]*.85f&&moved[2]<wet[1]*1.15f,"compression follows new output");
    juce::MemoryBlock state; p.getStateInformation(state);
    Processor restored; restored.prepareToPlay(44100,256); restored.setStateInformation(state.getData(),int(state.getSize()));waitReady(restored);
    require(std::abs(restored.compressorValue(0,lsfx::compMix)-100)<.01f&&std::abs(restored.compressorValue(0,lsfx::compThreshold)+60)<.01f,"compressor state restored");
    require(restored.compressorValue(1,lsfx::compMix)==0,"other channel stays bypassed");
    restored.initializeInstance();waitReady(restored,false);
    require(restored.compressorValue(0,lsfx::compMix)==0,"initialize resets compressor");
    std::cout<<"PASS: "<<(sfz?"SFZ":"SF2 experimental")<<" channel compressor, routing move, independence, state\n";
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
        const float unityPeak=peak[1];
        p.allOff(); render(p,midi,12);
        p.setInstrumentGain(pack(0,3),-12.0f); render(p,midi,8); // allow the gain ramp to settle
        require(p.instrumentGain(pack(0,3))==-12.0f&&p.instrumentGain(pack(0,4))==0.0f,"independent address gains");
        require(p.channelGainParameter(1).get()==0.0f,"instrument gain does not alter channel gain");
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),11);
        peak=render(p,midi);
        require(peak[1]>unityPeak*0.20f&&peak[1]<unityPeak*0.32f,"SFZ instrument gain applied to audio");
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
        p.setInstrumentGain(pack(0,4),-3.5f);
        juce::MemoryBlock state; p.getStateInformation(state);
        Processor restored; restored.prepareToPlay(44100,256); restored.setStateInformation(state.getData(),int(state.getSize())); waitReady(restored);
        const auto restoredView=restored.getView();
        require(findPreset(restoredView.presets,pack(0,3))&&findPreset(restoredView.presets,pack(0,4)),"SFZ map persisted");
        require(restored.current(1)==pack(0,4)&&restored.current(2)==pack(0,4),"channel addresses persisted");
        require(restored.instrumentGain(pack(0,3))==-12.0f&&restored.instrumentGain(pack(0,4))==-3.5f,"per-program SFZ gains persisted");
        require(restored.channelGainParameter(1).get()==0.0f,"channel gain persisted independently");
        midi.addEvent(juce::MidiMessage::noteOn(2,60,juce::uint8(100)),0);
        midi.addEvent(juce::MidiMessage::noteOn(3,60,juce::uint8(100)),0);
        peak=render(restored,midi);
        require(peak[1]>1e-5f&&peak[2]>1e-5f&&peak[0]<1e-5f,"restored hybrid playback");
        require(restored.importSFZ(one,pack(0,4),2).wasOk(),"replace mapped SFZ"); waitReady(restored);
        require(restored.instrumentGain(pack(0,4))==0.0f&&restored.instrumentGain(pack(0,3))==-12.0f,"only replaced address gain reset");
        restored.initializeInstance(); waitReady(restored,false); require(restored.getView().presets.empty(),"initialize clears map");
        require(restored.instrumentGain(pack(0,3))==0.0f,"initialize clears SFZ gains");
        std::cout<<"PASS: SF2 + two SFZs, separate routing, duplicate source, state restore, initialize\n";
        testChannelCompression(one,true);
        if constexpr(sf2MultiOutput) {testChannelCompression(sf2,false);testAllSf2Outputs(sf2);}
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
