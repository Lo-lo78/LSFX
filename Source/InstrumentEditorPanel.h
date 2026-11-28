// Included inside Plugin.cpp's private namespace, after Keyed and speak.
class InstrumentEditorPanel final : public juce::Component, private juce::Timer, private juce::FocusChangeListener {
    using Project=lsfx::instrument::Project;
    using Format=lsfx::instrument::Format;
    using Mapping=lsfx::instrument::Mapping;
    using Zone=lsfx::instrument::Zone;
    using Draft=lsfx::instrument::Draft;
    using Button=Keyed<juce::TextButton>;
    struct JobResult {std::optional<Project> project;juce::File file;juce::String message;};
public:
    InstrumentEditorPanel(Draft& d,std::function<void()> close,std::function<juce::Result(const juce::File&)> apply,juce::String target,std::function<void(const Project&,int)> preview,std::function<juce::String()> previewStatus,std::function<void()> panic)
        :draft(d),closeAction(std::move(close)),applyAction(std::move(apply)),targetLabel(std::move(target)),previewAction(std::move(preview)),statusAction(std::move(previewStatus)),panicAction(std::move(panic)) {
        setWantsKeyboardFocus(false);setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        setup(format,"Format",10,[this]{});setup(name,"Instrument name",11);name.setMultiLine(false);name.setInputRestrictions(60);name.setTabKeyUsedAsCharacter(false);name.setText(lsfx::instrument::js(draft.project.name),false);
        name.onTextChange=[this]{if(!busy){auto n=name.getText().trim();if(n.isNotEmpty()&&n.getNumBytesAsUTF8()<=256&&draft.project.name!=lsfx::instrument::ss(n)){draft.project.name=lsfx::instrument::ss(n);draft.dirty=true;}}};
        name.onFocusLost=[this]{name.setText(lsfx::instrument::js(draft.project.name),false);};
        setup(newProject,"New project",12,[this]{newDraft();});setup(openProject,"Open project",13,[this]{browseOpen();});
        setup(sampleCell,"Samples",10,[this]{browseImport();});setup(importButton,"Import audio samples",11,[this]{browseImport();});setup(mappingMode,"Import mapping mode",12,[this]{changeMode(1);});setup(chromaticStart,"Chromatic start note",13,[this]{changeStart(1);});setup(removeSample,"Remove unused sample",14,[this]{deleteSample();});setup(addZoneButton,"Add selected sample as zone",15,[this]{addZone();});
        setup(zoneCell,"Mapping grid",10,[this]{say(zoneCell,zoneText());});setup(duplicateZone,"Duplicate zone",11,[this]{duplicate();});setup(deleteZone,"Delete zone",12,[this]{deleteCurrentZone();});
        setup(saveButton,"Save project",10,[this]{save(false);});setup(saveAsButton,"Save project as",11,[this]{save(true);});setup(exportButton,"Export instrument",12,[this]{exportFile(false);});setup(applyButton,"Export and replace current program",13,[this]{exportFile(true);});
        setup(closeButton,"Close instrument editor",30,[this]{closeEditor();});
        details.setAccessible(false);details.setJustificationType(juce::Justification::centredLeft);addAndMakeVisible(details);
        message.setAccessible(false);message.setTitle("Instrument editor status");message.setJustificationType(juce::Justification::centredLeft);addAndMakeVisible(message);
        const auto& pos=draft.position;page=juce::jlimit(0,3,pos.page);sampleIndex=pos.sample;zoneIndex=pos.zone;column=pos.parameter;modeIndex=juce::jlimit(0,3,pos.mode);startNote=juce::jlimit(0,127,pos.start);stepIndex=juce::jlimit(0,int(std::size(steps))-1,pos.step);lastFocus=pos.focus;
        updateAll();showPage(page,false);juce::Desktop::getInstance().addFocusChangeListener(this);
    }
    ~InstrumentEditorPanel() override {rememberPosition();juce::Desktop::getInstance().removeFocusChangeListener(this);stopTimer();chooser.reset();if(worker.joinable())worker.join();}
    void activate(){showPage(page,false);auto* c=rememberedControl();focus(*c);say(*c,c==&zoneCell?zoneContext()+", "+zoneText():c==&sampleCell?sampleText():c->getTitle());refreshPreview();startTimerHz(5);}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff202429));
        if(page!=2||p().zones.empty()||gridBounds.isEmpty())return;
        const int first=(column/16)*16,w=gridBounds.getWidth()/2,h=gridBounds.getHeight()/8;
        auto params=parameters();g.setFont(juce::FontOptions(13.0f));
        for(int i=first;i<std::min(first+16,int(params.size()));++i){int k=i-first;auto cell=juce::Rectangle<int>(gridBounds.getX()+(k/8)*w,gridBounds.getY()+(k%8)*h,w,h).reduced(2);
            g.setColour(i==column?juce::Colour(0xff3d536c):juce::Colour(0xff2b3036));g.fillRoundedRectangle(cell.toFloat(),3);g.setColour(juce::Colours::white);g.drawFittedText(parameterText(i),cell.reduced(5,1),juce::Justification::centredLeft,1);}
    }
    void resized() override {
        auto r=getLocalBounds().reduced(12);gridBounds={};
        r.removeFromTop(8);message.setBounds(r.removeFromTop(44));details.setBounds(r.removeFromTop(50));r.removeFromTop(8);
        closeButton.setBounds(r.removeFromBottom(40).removeFromRight(220));
        auto put=[&](juce::Component& c){c.setBounds(r.removeFromTop(40).reduced(0,3));};
        if(page==0){put(format);put(name);put(newProject);put(openProject);}
        else if(page==1){put(sampleCell);put(importButton);put(mappingMode);put(chromaticStart);put(removeSample);put(addZoneButton);}
        else if(page==2){put(zoneCell);auto actions=r.removeFromBottom(40);duplicateZone.setBounds(actions.removeFromLeft(actions.getWidth()/2).reduced(2));deleteZone.setBounds(actions.reduced(2));gridBounds=r;}
        else {put(saveButton);put(saveAsButton);put(exportButton);put(applyButton);}
    }
private:
    Draft& draft;std::function<void()> closeAction;std::function<juce::Result(const juce::File&)> applyAction;juce::String targetLabel;
    std::function<void(const Project&,int)> previewAction;std::function<juce::String()> statusAction;std::function<void()> panicAction;
    int rawSample=-1;juce::String previousPreviewStatus;
    void refreshPreview(){rawSample=-1;previewAction(p(),-1);}
    void auditionSample(){if(p().samples.empty())return;if(rawSample==sampleIndex){refreshPreview();notify("Mapped MIDI preview");}else{rawSample=sampleIndex;previewAction(p(),rawSample);notify("Audition sample "+lsfx::instrument::js(p().samples[size_t(sampleIndex)].name));}}
    void timerCallback() override {auto status=statusAction();if(status==previousPreviewStatus)return;previousPreviewStatus=status;if(status=="Preview: sample ended"&&rawSample>=0){refreshPreview();return;}if(status.startsWith("Preview error")){if(!busy&&!chooser)notify(status);} }
    Button format,newProject,openProject,sampleCell,importButton,mappingMode,chromaticStart,removeSample,addZoneButton,zoneCell,duplicateZone,deleteZone,saveButton,saveAsButton,exportButton,applyButton,closeButton;
    Keyed<juce::TextEditor> name;
    juce::Label message,details;juce::Rectangle<int> gridBounds;
    int page=0,sampleIndex=0,zoneIndex=0,column=0,modeIndex=0,startNote=60,stepIndex=0,lastFocus=10;
    bool busy=false;
    std::vector<Project>& undo=draft.undo;std::vector<Project>& redo=draft.redo;std::optional<Zone>& clipboard=draft.clipboard;
    void globalFocusChanged(juce::Component* c) override {if(c&&isParentOf(c)&&c->getExplicitFocusOrder()>0)lastFocus=c->getExplicitFocusOrder();}
    void rememberPosition(){draft.position={page,sampleIndex,zoneIndex,column,modeIndex,startNote,stepIndex,lastFocus};}
    juce::Component* rememberedControl(){for(int i=0;i<getNumChildComponents();++i){auto* c=getChildComponent(i);if(c->isVisible()&&c->getWantsKeyboardFocus()&&c->getExplicitFocusOrder()==lastFocus)return c;}return &primary();}
    std::unique_ptr<juce::FileChooser> chooser;std::thread worker;
    static constexpr int steps[]={1,5,10,20,50,100,500,1000,10000};
    template<class C> void setup(Keyed<C>& c,const juce::String& text,int order){c.setName(text);c.setTitle(text);c.setExplicitFocusOrder(order);c.setWantsKeyboardFocus(true);c.keyHandler=[this](const juce::KeyPress& k,juce::Component* origin){return handle(k,origin);};addAndMakeVisible(c);}
    void setup(Button& b,const juce::String& text,int order,std::function<void()> action){setup<juce::TextButton>(b,text,order);b.setButtonText(text);b.onClick=[this,action=std::move(action)]{if(!busy&&!chooser)action();};}
    static void label(Button& b,const juce::String& text){if(b.getButtonText()!=text){b.setButtonText(text);b.setName(text);b.setTitle(text);}}
    void say(juce::Component& c,const juce::String& text){speak(c,text);}
    void focus(juce::Component& c){if(c.isShowing()){lastFocus=c.getExplicitFocusOrder();c.grabKeyboardFocus();}}
    juce::Component& primary(){if(page==0)return format;if(page==1)return sampleCell;if(page==2)return zoneCell;return saveButton;}
    void notify(const juce::String& text){message.setText(text,juce::dontSendNotification);message.setTitle(text);if(auto* c=juce::Component::getCurrentlyFocusedComponent();c&&isParentOf(c))say(*c,text);}
    void focusAfterDialog(){focus(primary());juce::Component::SafePointer<InstrumentEditorPanel> safe(this);juce::Timer::callAfterDelay(150,[safe]{if(auto* p=safe.getComponent();p&&p->isShowing()&&p->getPeer()&&p->getPeer()->isFocused()&&!p->chooser){auto* c=juce::Component::getCurrentlyFocusedComponent();if(!c||!p->isParentOf(c))p->focus(p->primary());}});}
    void showPage(int n,bool grab=true){if(busy&&grab)return;if(n!=1&&rawSample>=0)refreshPreview();page=juce::jlimit(0,3,n);std::array<juce::Component*,4> a{&format,&name,&newProject,&openProject};std::array<juce::Component*,6> b{&sampleCell,&importButton,&mappingMode,&chromaticStart,&removeSample,&addZoneButton};std::array<juce::Component*,3> c{&zoneCell,&duplicateZone,&deleteZone};std::array<juce::Component*,4> d{&saveButton,&saveAsButton,&exportButton,&applyButton};for(auto* p:a)p->setVisible(page==0);for(auto* p:b)p->setVisible(page==1);for(auto* p:c)p->setVisible(page==2);for(auto* p:d)p->setVisible(page==3);updateAll();resized();if(grab){focus(primary());say(primary(),page==0?format.getButtonText():page==1?sampleText():page==2?zoneContext()+", "+zoneText():targetLabel);}}
    Project& p(){return draft.project;}const Project& p() const{return draft.project;}
    juce::String sampleText() const {if(p().samples.empty())return "No samples. Enter to import";auto& s=p().samples[size_t(sampleIndex)];return "Sample "+juce::String(sampleIndex+1)+" of "+juce::String(int(p().samples.size()))+", "+lsfx::instrument::js(s.name)+", Root "+juce::String(s.root);}
    using Param=lsfx::instrument::ZoneParam;
    std::vector<const Param*> parameters() const {std::vector<const Param*> out;for(const auto& param:lsfx::instrument::zoneParams)if((!param.sfzOnly||p().format==Format::sfz)&&(!param.sf2Only||p().format==Format::sf2))out.push_back(&param);return out;}
    juce::String zoneContext() const {return p().zones.empty()?juce::String("No zones"):"Zone "+juce::String(zoneIndex+1)+" of "+juce::String(int(p().zones.size()));}
    juce::String parameterText(int i) const {
        if(p().zones.empty())return "No zones";
        const auto params=parameters();if(i<0||i>=int(params.size()))return {};
        const auto& param=*params[size_t(i)];const auto& z=p().zones[size_t(zoneIndex)];const int n=z.*param.member;juce::String v;
        if(param.member==&Zone::sample)v=lsfx::instrument::js(p().samples[size_t(n)].name);
        else if(param.member==&Zone::loopMode){static const char* modes[]={"Off","Continuous","Sustain","One shot"};v=modes[std::clamp(n,0,3)];}
        else if(param.member==&Zone::trigger){static const char* names[]={"Attack","Release","First","Legato"};v=names[n];}
        else if(param.member==&Zone::offMode){static const char* names[]={"Fast","Normal","Time"};v=names[n];}
        else if(param.member==&Zone::filterType){static const char* names[]={"Low pass 2 pole","High pass 2 pole","Band pass 2 pole","Band reject 2 pole","Low pass 1 pole","High pass 1 pole"};v=names[n];}
        else if(param.member==&Zone::sustainSwitch)v=n?"On":"Off";
        else if(param.member==&Zone::notePolyphony&&n==0)v="Unlimited";
        else if(juce::String(param.unit)=="0.1 percent")v=juce::String(n/10.0,1)+" percent";
        else if(param.member==&Zone::pan)v=n==0?juce::String("Center"):juce::String(std::abs(n))+" percent "+(n<0?"left":"right");
        else if((param.member==&Zone::endFrame||param.member==&Zone::loopEnd)&&n<0)v="End";
        else if((param.member==&Zone::choke||param.member==&Zone::chokeTarget)&&n==0)v="Off";
        else if(juce::String(param.unit)=="mHz")v=juce::String(n/1000.0,3)+" Hz";
        else if(juce::String(param.unit)=="0.1 dB")v=juce::String(n/10.0,1)+" dB";
        else {v=juce::String(n);if(*param.unit)v+=" "+juce::String(param.unit);}
        return juce::String(param.name)+", "+v;
    }
    juce::String zoneText() const {return p().zones.empty()?juce::String("No mapping zones. Import audio on F2"):parameterText(column);}
    void syncSample(){if(!p().zones.empty())sampleIndex=p().zones[size_t(std::clamp(zoneIndex,0,int(p().zones.size())-1))].sample;}
    
    void moveZone(int d,juce::Component* origin){if(p().zones.empty())return;int n=std::clamp(zoneIndex+d,0,int(p().zones.size())-1);if(n==zoneIndex)return;zoneIndex=n;if(rawSample>=0)refreshPreview();updateAll();say(*origin,zoneContext()+", "+(page==2?zoneText():lsfx::instrument::js(p().samples[size_t(p().zones[size_t(zoneIndex)].sample)].name)));}
    void updateAll(){sampleIndex=juce::jlimit(0,juce::jmax(0,int(p().samples.size())-1),sampleIndex);zoneIndex=juce::jlimit(0,juce::jmax(0,int(p().zones.size())-1),zoneIndex);column=juce::jlimit(0,int(parameters().size())-1,column);
        label(format,p().format==Format::sf2?"Format: SF2":"Format: SFZ");label(sampleCell,sampleText());label(zoneCell,zoneText());
        static const char* modes[]={"Layer","Chromatic","Multisample","Velocity layers"};label(mappingMode,juce::String("Import mapping: ")+modes[modeIndex]);label(chromaticStart,"Chromatic start note: "+juce::String(startNote));label(applyButton,"Export and replace "+targetLabel);
        details.setText((page>=1?zoneContext()+". ":juce::String{})+juce::String(int(p().samples.size()))+" unique samples, "+juce::String(int(p().zones.size()))+" zones, "+juce::String(double(p().bytes())/1048576.0,2)+" MiB source audio"+(draft.dirty?". Unsaved changes.":"."),juce::dontSendNotification);repaint();
    }
    void pruneHistory(){while(undo.size()>24)undo.erase(undo.begin());while(redo.size()>24)redo.erase(redo.begin());auto total=[&]{std::set<const lsfx::instrument::Blob*> seen;size_t n=0;auto scan=[&](const Project& q){for(auto& s:q.samples)if(s.bytes&&seen.insert(s.bytes.get()).second)n+=s.bytes->size();};scan(p());for(auto& q:undo)scan(q);for(auto& q:redo)scan(q);return n;};while(total()>lsfx::instrument::maxProjectBytes){if(!undo.empty())undo.erase(undo.begin());else if(!redo.empty())redo.erase(redo.begin());else break;}}
    void snapshot(){undo.push_back(p());redo.clear();pruneHistory();}
    void changed(){draft.dirty=true;pruneHistory();updateAll();refreshPreview();}
    void history(bool forward){auto& a=forward?redo:undo;auto& b=forward?undo:redo;if(a.empty())return;b.push_back(p());p()=std::move(a.back());a.pop_back();name.setText(lsfx::instrument::js(p().name),false);changed();notify(forward?"Redo":"Undo");}
    void toggleFormat(){snapshot();p().format=p().format==Format::sf2?Format::sfz:Format::sf2;if(p().format==Format::sf2)for(auto& z:p().zones)if(z.loopMode==3)z.loopMode=0;changed();say(format,format.getButtonText());}
    void changeMode(int d){const int n=juce::jlimit(0,3,modeIndex+d);if(n==modeIndex)return;modeIndex=n;updateAll();say(mappingMode,mappingMode.getButtonText());}
    void changeStart(int d){const int n=juce::jlimit(0,127,startNote+d);if(n==startNote)return;startNote=n;updateAll();say(chromaticStart,chromaticStart.getButtonText());}
    void newDraft(){if(draft.dirty&&(!p().samples.empty()||draft.savedFile!=juce::File{})){notify("Save the current project before creating a new one");return;}auto fmt=p().format;draft=Draft{};p().format=fmt;undo.clear();redo.clear();clipboard.reset();sampleIndex=zoneIndex=column=0;name.setText(lsfx::instrument::js(p().name),false);updateAll();refreshPreview();notify("New empty instrument project");}
    void closeEditor(){if(busy||chooser)return;rememberPosition();auto callback=closeAction;if(callback)callback();}
    void deleteSample(){if(p().samples.empty())return;auto copy=p();if(!copy.removeSample(size_t(sampleIndex))){notify("Sample is used by a mapping zone. Delete its zones first");return;}snapshot();p()=std::move(copy);changed();notify(sampleText());}
    void addZone(){if(p().samples.empty())return;if(p().zones.size()>=lsfx::instrument::maxZones){notify("Zone limit reached");return;}snapshot();p().addZones({sampleIndex},Mapping::layer);zoneIndex=int(p().zones.size())-1;column=0;changed();showPage(2);}
    void duplicate(){if(p().zones.empty())return;if(p().zones.size()>=lsfx::instrument::maxZones){notify("Zone limit reached");return;}auto z=p().zones[size_t(zoneIndex)];snapshot();p().zones.insert(p().zones.begin()+zoneIndex+1,z);++zoneIndex;changed();focus(zoneCell);say(zoneCell,zoneText());}
    void deleteCurrentZone(){if(p().zones.empty())return;snapshot();p().zones.erase(p().zones.begin()+zoneIndex);changed();focus(zoneCell);say(zoneCell,zoneText());}
    void navigateSample(int code,bool ctrl){if(p().samples.empty())return;int n=sampleIndex;if(code==juce::KeyPress::upKey)--n;else if(code==juce::KeyPress::downKey)++n;else if(code==juce::KeyPress::pageUpKey)n-=5;else if(code==juce::KeyPress::pageDownKey)n+=5;else if(code==juce::KeyPress::homeKey)n=0;else if(code==juce::KeyPress::endKey)n=int(p().samples.size())-1;else return;(void)ctrl;n=juce::jlimit(0,int(p().samples.size())-1,n);if(n==sampleIndex)return;sampleIndex=n;if(rawSample>=0)refreshPreview();updateAll();say(sampleCell,sampleText());}
    void navigateZone(int code,bool ctrl){if(p().zones.empty())return;using Move=lsfx::GridMove;Move m;
        if(code==juce::KeyPress::upKey)m=Move::up;else if(code==juce::KeyPress::downKey)m=Move::down;else if(code==juce::KeyPress::leftKey)m=Move::left;else if(code==juce::KeyPress::rightKey)m=Move::right;else if(code==juce::KeyPress::pageUpKey)m=Move::pageUp;else if(code==juce::KeyPress::pageDownKey)m=Move::pageDown;else if(code==juce::KeyPress::homeKey)m=Move::home;else if(code==juce::KeyPress::endKey)m=Move::end;else return;
        int n=lsfx::gridDestination(column,int(parameters().size()),8,m,ctrl);if(n==column)return;column=n;updateAll();say(zoneCell,zoneText());}
    void editZone(int code){if(p().zones.empty())return;if(code==juce::KeyPress::leftKey||code==juce::KeyPress::rightKey){int n=juce::jlimit(0,int(std::size(steps))-1,stepIndex+(code==juce::KeyPress::rightKey?1:-1));if(n!=stepIndex){stepIndex=n;say(zoneCell,"Step "+juce::String(steps[n]));}return;}
        auto z=p().zones[size_t(zoneIndex)];const auto& param=*parameters()[size_t(column)];auto member=param.member;int lo=param.lo,hi=param.hi;
        const auto& sample=p().samples[size_t(z.sample)];int last=lsfx::instrument::lastFrame(sample),end=lsfx::instrument::effectiveEnd(z,sample);
        if(member==&Zone::sample)hi=int(p().samples.size())-1;else if(member==&Zone::low)hi=z.high;else if(member==&Zone::high)lo=z.low;else if(member==&Zone::lowVel)hi=z.highVel;else if(member==&Zone::highVel)lo=z.lowVel;
        else if(member==&Zone::seqPosition)hi=z.seqLength;else if(member==&Zone::seqLength)lo=z.seqPosition;else if(member==&Zone::randomLow)hi=z.randomHigh;else if(member==&Zone::randomHigh)lo=z.randomLow;else if(member==&Zone::offsetRandom)hi=end-z.startFrame;
        else if(member==&Zone::loopMode)hi=p().format==Format::sf2?2:3;else if(member==&Zone::startFrame)hi=end;else if(member==&Zone::endFrame)hi=last;else if(member==&Zone::loopStart){lo=z.startFrame;hi=lsfx::instrument::effectiveLoopEnd(z,sample);}else if(member==&Zone::loopEnd)hi=end;
        int n=z.*member,step=(member==&Zone::sample||member==&Zone::loopMode||member==&Zone::trigger||member==&Zone::offMode||member==&Zone::filterType||member==&Zone::sustainSwitch)?1:steps[stepIndex];if(code==juce::KeyPress::backspaceKey)n=param.def;else if(code==juce::KeyPress::homeKey)n=hi;else if(code==juce::KeyPress::endKey)n=lo;else if(code==juce::KeyPress::upKey)n+=step;else if(code==juce::KeyPress::downKey)n-=step;else if(code==juce::KeyPress::pageUpKey)n+=step*10;else if(code==juce::KeyPress::pageDownKey)n-=step*10;else return;n=juce::jlimit(lo,hi,n);if(n==z.*member)return;z.*member=n;
        if(member==&Zone::sample){z.startFrame=z.loopStart=z.offsetRandom=0;z.endFrame=z.loopEnd=-1;}lsfx::instrument::clampFrames(z,p().samples[size_t(z.sample)]);snapshot();p().zones[size_t(zoneIndex)]=z;changed();say(zoneCell,zoneText());}
    bool handle(const juce::KeyPress& k,juce::Component* origin){if(chooser)return false;const int key=k.getKeyCode();auto mods=k.getModifiers();const bool alt=mods.isAltDown(),ctrl=mods.isCtrlDown()||mods.isCommandDown(),shift=mods.isShiftDown();const int ch=juce::CharacterFunctions::toLowerCase(juce::juce_wchar(key));
        if(busy)return true;
        if(!alt&&!ctrl){if(key==juce::KeyPress::F1Key){showPage(0);return true;}if(key==juce::KeyPress::F2Key){showPage(1);return true;}if(key==juce::KeyPress::F3Key){showPage(2);return true;}if(key==juce::KeyPress::F4Key){showPage(3);return true;}if(key==juce::KeyPress::escapeKey){closeEditor();return true;}}
        if(ctrl&&!alt&&key==juce::KeyPress::tabKey){showPage((page+(shift?3:1))%4);return true;}
        if(ctrl&&!alt&&key==juce::KeyPress::returnKey){panicAction();return true;}
        if(ctrl&&!alt&&ch=='s'){save(shift);return true;}
        if(origin!=&name&&ctrl&&!alt&&(ch=='z'||ch=='y')){history(ch=='y'||shift);return true;}
        if(alt&&!ctrl&&ch=='i'){showPage(1);browseImport();return true;}
        const bool nav=key==juce::KeyPress::upKey||key==juce::KeyPress::downKey||key==juce::KeyPress::leftKey||key==juce::KeyPress::rightKey||key==juce::KeyPress::pageUpKey||key==juce::KeyPress::pageDownKey||key==juce::KeyPress::homeKey||key==juce::KeyPress::endKey;
        if(page>=1&&ctrl&&!alt&&(key==juce::KeyPress::upKey||key==juce::KeyPress::downKey)){moveZone(key==juce::KeyPress::upKey?-1:1,origin);return true;}
        if(origin==&format){if(!alt&&!ctrl&&key==juce::KeyPress::downKey&&p().format==Format::sf2)toggleFormat();else if(!alt&&!ctrl&&key==juce::KeyPress::upKey&&p().format==Format::sfz)toggleFormat();if(nav||key==juce::KeyPress::spaceKey||key==juce::KeyPress::returnKey)return true;}
        if(origin==&mappingMode&&!ctrl&&nav){changeMode(key==juce::KeyPress::upKey||key==juce::KeyPress::rightKey?1:-1);return true;}
        if(origin==&chromaticStart&&!ctrl&&nav){changeStart(key==juce::KeyPress::upKey||key==juce::KeyPress::rightKey?1:-1);return true;}
        if(origin==&sampleCell){if(!ctrl&&!alt&&key==juce::KeyPress::spaceKey){auditionSample();return true;}if(!alt&&nav){navigateSample(key,ctrl);return true;}if(!ctrl&&!alt&&key==juce::KeyPress::returnKey){browseImport();return true;}if(!ctrl&&!alt&&key==juce::KeyPress::deleteKey){deleteSample();return true;}if(!ctrl&&!alt&&key==juce::KeyPress::insertKey){addZone();return true;}}
        if(origin==&zoneCell){if(!ctrl&&!alt&&key==juce::KeyPress::backspaceKey){editZone(key);return true;}if(nav){if(alt&&!ctrl)editZone(key);else if(!alt)navigateZone(key,ctrl);return true;}if(ctrl&&!alt&&ch=='d'){duplicate();return true;}if(ctrl&&!alt&&ch=='c'){if(!p().zones.empty()){clipboard=p().zones[size_t(zoneIndex)];notify("Zone settings copied");}return true;}if(ctrl&&!alt&&ch=='v'){if(clipboard&&!p().zones.empty()){auto z=*clipboard;z.sample=p().zones[size_t(zoneIndex)].sample;if(p().format==Format::sf2&&z.loopMode==3)z.loopMode=0;lsfx::instrument::clampFrames(z,p().samples[size_t(z.sample)]);snapshot();p().zones[size_t(zoneIndex)]=z;changed();say(zoneCell,zoneText());}return true;}if(!ctrl&&!alt&&key==juce::KeyPress::insertKey){addZone();return true;}if(!ctrl&&!alt&&key==juce::KeyPress::deleteKey){deleteCurrentZone();return true;}if(!ctrl&&!alt&&(key==juce::KeyPress::returnKey||key==juce::KeyPress::spaceKey)){say(zoneCell,zoneText());return true;}}
        if(!alt&&!ctrl&&key==juce::KeyPress::tabKey){std::vector<juce::Component*> controls;if(page==0)controls={&format,&name,&newProject,&openProject};else if(page==1)controls={&sampleCell,&importButton,&mappingMode,&chromaticStart,&removeSample,&addZoneButton};else if(page==2)controls={&zoneCell,&duplicateZone,&deleteZone};else controls={&saveButton,&saveAsButton,&exportButton,&applyButton};controls.push_back(&closeButton);auto it=std::find(controls.begin(),controls.end(),origin);int i=it==controls.end()?0:int(it-controls.begin());focus(*controls[size_t((i+int(controls.size())+(shift?-1:1))%int(controls.size()))]);return true;}
        if(origin==&name&&(key==juce::KeyPress::upKey||key==juce::KeyPress::downKey||key==juce::KeyPress::pageUpKey||key==juce::KeyPress::pageDownKey))return true;
        if(auto* b=dynamic_cast<Button*>(origin)){if(nav)return true;if(!alt&&!ctrl&&(key==juce::KeyPress::spaceKey||key==juce::KeyPress::returnKey)){b->triggerClick();return true;}}
        return false;
    }
    juce::File directory(){auto d=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("LSFX").getChildFile("Instruments");d.createDirectory();return d;}
    juce::String filename(){auto n=juce::File::createLegalFileName(lsfx::instrument::js(p().name)).trim();return n.isEmpty()||n=="."||n==".."?juce::String("New Instrument"):n;}
    void runJob(std::function<JobResult()> task,std::function<void(JobResult&&)> finish,const juce::String& text){if(busy)return;if(worker.joinable())worker.join();busy=true;notify(text);juce::Component::SafePointer<InstrumentEditorPanel> safe(this);
        try{worker=std::thread([safe,task=std::move(task),finish=std::move(finish)]() mutable {JobResult r;bool ok=false;try{r=task();ok=true;}catch(const std::exception& e){r.message=juce::String("Instrument editor: ")+juce::String::fromUTF8(e.what());}catch(...){r.message="Instrument editor operation failed";}
            juce::MessageManager::callAsync([safe,ok,r=std::move(r),finish=std::move(finish)]() mutable {if(auto* panel=safe.getComponent()){if(panel->worker.joinable())panel->worker.join();panel->busy=false;if(ok)finish(std::move(r));else panel->notify(r.message);panel->updateAll();panel->focusAfterDialog();}});
        });}catch(...){busy=false;notify("Cannot start instrument editor worker");}
    }
    void browseImport(){if(busy||chooser)return;chooser=std::make_unique<juce::FileChooser>("Import audio samples",directory(),"*.wav;*.aif;*.aiff;*.flac",true,false,this);juce::Component::SafePointer<InstrumentEditorPanel> safe(this);chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::canSelectMultipleItems,[safe](const juce::FileChooser& fc){if(auto* e=safe.getComponent()){auto files=fc.getResults();e->chooser.reset();if(files.isEmpty()){e->focusAfterDialog();return;}auto copy=e->p();auto mode=Mapping(e->modeIndex);int start=e->startNote;e->runJob([copy=std::move(copy),files,mode,start]() mutable {juce::AudioFormatManager formats;formats.registerBasicFormats();std::vector<int> ids;for(const auto& f:files)ids.push_back(copy.addSample(lsfx::instrument::importSample(formats,f)));copy.addZones(ids,mode,start);copy.validate();JobResult r;r.message=juce::String(int(ids.size()))+" mapping zones imported";r.project=std::move(copy);return r;},[e](JobResult&& r){e->snapshot();e->p()=std::move(*r.project);e->zoneIndex=int(e->p().zones.size())-1;e->syncSample();e->changed();e->notify(r.message);},"Importing audio samples");}});}
    void browseOpen(){if(busy||chooser)return;if(draft.dirty&&(!p().samples.empty()||draft.savedFile!=juce::File{})){notify("Save current project before opening another");return;}chooser=std::make_unique<juce::FileChooser>("Open instrument project",directory(),"*.lsfx-instrument",true,false,this);juce::Component::SafePointer<InstrumentEditorPanel> safe(this);chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& fc){if(auto* e=safe.getComponent()){auto f=fc.getResult();e->chooser.reset();if(f==juce::File{}){e->focusAfterDialog();return;}e->runJob([f]{JobResult r;r.project=lsfx::instrument::openProject(f);r.file=f;r.message="Instrument project opened";return r;},[e](JobResult&& r){e->p()=std::move(*r.project);e->draft.savedFile=r.file;e->draft.dirty=false;e->undo.clear();e->redo.clear();e->clipboard.reset();e->sampleIndex=e->zoneIndex=e->column=0;e->syncSample();e->name.setText(lsfx::instrument::js(e->p().name),false);e->updateAll();e->refreshPreview();e->notify(r.message);},"Opening instrument project");}});}
    void save(bool as){if(busy||chooser)return;if(!as&&draft.savedFile!=juce::File{}){saveTo(draft.savedFile,true);return;}chooser=std::make_unique<juce::FileChooser>("Save instrument project",directory().getChildFile(filename()+".lsfx-instrument"),"*.lsfx-instrument",true,false,this);juce::Component::SafePointer<InstrumentEditorPanel> safe(this);chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& fc){if(auto* e=safe.getComponent()){auto f=fc.getResult();e->chooser.reset();if(f==juce::File{}){e->focusAfterDialog();return;}e->saveTo(f.withFileExtension("lsfx-instrument"),false);}});}
    void saveTo(const juce::File& file,bool replace){auto copy=p();runJob([copy=std::move(copy),file,replace]{lsfx::instrument::saveProject(copy,file,replace);JobResult r;r.file=file;r.message="Instrument project saved";return r;},[this](JobResult&& r){draft.savedFile=r.file;draft.dirty=false;notify(r.message);},"Compressing instrument project");}
    void exportFile(bool apply){if(busy||chooser)return;if(p().zones.empty()){notify("Import audio samples and create mapping zones first");return;}auto ext=p().format==Format::sf2?".sf2":".sfz";chooser=std::make_unique<juce::FileChooser>(apply?"Export and load instrument":"Export instrument",directory().getChildFile(filename()+ext),juce::String("*")+ext,true,false,this);juce::Component::SafePointer<InstrumentEditorPanel> safe(this);chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,[safe,apply](const juce::FileChooser& fc){if(auto* e=safe.getComponent()){auto f=fc.getResult();e->chooser.reset();if(f==juce::File{}){e->focusAfterDialog();return;}auto copy=e->p();f=f.withFileExtension(copy.format==Format::sf2?"sf2":"sfz");e->runJob([copy=std::move(copy),f]{JobResult r;r.file=lsfx::instrument::exportInstrument(copy,f);r.message="Exported "+r.file.getFileName();return r;},[e,apply](JobResult&& r){if(apply){auto result=e->applyAction(r.file);e->notify(result.failed()?r.message+". "+result.getErrorMessage():r.message+". Loading in selected program");}else e->notify(r.message);},"Exporting instrument");}});}
};
