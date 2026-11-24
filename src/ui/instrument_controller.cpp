#include <cell/instrument_controller.hpp>
#include <cell/ui_components.hpp>
#include <charconv>
#include <cctype>
#include <chrono>
#include <optional>
#include <utility>

namespace cell::ui {
namespace {
constexpr Color face(0xffc0c0c0),white(0xffffffff),light(0xffdfdfdf),shadow(0xff808080),black(0xff000000),navy(0xff000080),blue(0xff1084d0),active(0xff75d3cb);
constexpr std::uint64_t identity=0x434b425700000000ull;
enum Button:unsigned {closeButton=1,maximizeButton,keysButton,padsButton,channelButton,velocityButton,octaveDown,octaveUp,centreButton,panicButton,deviceButton,learnButton,learnAll,learnApply,learnCancel,presetPrev,presetNext,presetName,presetFormat,presetSave,presetDefault,presetUse,presetDisable,presetClose};
// Keep existing controller and accessibility identities stable.
constexpr unsigned minimizeButton=25,restoreButton=26;
bool darkKey(int note){const auto n=note%12;return n==1||n==3||n==6||n==8||n==10;}
std::string noteName(int note){static constexpr const char* names[]{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};return std::string(names[note%12])+std::to_string(note/12-1);}
int lower(int key){return key>='A'&&key<='Z'?key+('a'-'A'):key;}
void label(Canvas& c,Rect box,std::string_view text,double size=11,Color color=black){
    const double width=std::max(0.,box.width-5);
    if(c.textWidth(text,size,500)<=width){components::centeredText(c,box,text,size,color);return;}
    // Keep the chosen font proportions even in small pads or with long names.
    // Full names stay in the control metadata for accessibility and inspection.
    const double available=std::max(0.,width-c.textWidth("...",size,500));
    auto boundary=[&](std::size_t end){while(end&&end<text.size()&&(static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;return end;};
    std::size_t first=0,last=text.size();while(first<last){const auto mid=first+(last-first+1)/2;if(c.textWidth(text.substr(0,boundary(mid)),size,500)<=available)first=mid;else last=mid-1;}
    components::centeredText(c,box,std::string(text.substr(0,boundary(first)))+"...",size,color);
}
void bevel(Canvas& c,Rect r,bool down=false){c.fill(r,face);c.line({r.x,r.y+r.height-1},{r.x,r.y},down?shadow:white);c.line({r.x,r.y},{r.x+r.width-1,r.y},down?shadow:white);c.line({r.x+1,r.y+r.height-1},{r.x+r.width-1,r.y+r.height-1},down?white:black);c.line({r.x+r.width-1,r.y+r.height-1},{r.x+r.width-1,r.y+1},down?white:black);}
struct Hit {Rect box;unsigned button{};int note=-1;std::uint64_t id{};};
}
struct InstrumentController::Impl {
    Hooks hooks;std::string title;bool open{},maximized{},minimized{},focused{},dirty=true;Mode mode=Mode::keyboard;
    Rect parent{},box{},restore{},titleBox{},grip{},keysBox{};std::array<std::optional<Rect>,2> modeBounds;unsigned channel=1,velocity=100,padBank=0,layoutBank=0;int computerBase=60,firstWhite=-1;std::array<PadInfo,16> padInfo;bool layoutKnown=false;
    std::vector<Hit> hits;std::vector<ControlInfo> controls;
    std::array<unsigned,128> held{};std::array<int,256> computer{};std::array<bool,256> accessibleHeld{};
    struct Contact {PointerIdentity pointer;int note=-1;};std::vector<Contact> contacts;
    struct Drag {PointerIdentity pointer;Point start;Rect original;bool resize{};};std::optional<Drag> drag;
    struct Press {PointerIdentity pointer;Hit hit;};std::optional<Press> press;
    struct Entry {unsigned button{};std::string text;bool replace=true;};std::optional<Entry> entry;
    std::string status;
    std::uint64_t focusId{};
    DeviceHooks device;std::unique_ptr<midi::PadProfileStore> store;midi::PadProfiles profiles;midi::PadLearner learner;std::optional<midi::PadProfile> selected,applied;
    std::uint64_t deviceOwner{},captureSequence{};bool devicePanel{},defaultPending{},formatKnown{},deviceWasMaximized{};std::optional<Rect> deviceRestore;Rect deviceNormalRestore;std::string format;std::chrono::steady_clock::time_point nextDevicePoll{};
    void showDevicePanel(bool show){
        if(show==devicePanel)return;
        if(show){deviceRestore=box;deviceNormalRestore=restore;deviceWasMaximized=maximized;devicePanel=true;maximized=false;box.height=264;box=clamped(box);restore=box;}
        else{devicePanel=false;maximized=deviceWasMaximized;restore=clamped(deviceNormalRestore);box=maximized?maximizedBox():clamped(deviceRestore.value_or(box));deviceRestore.reset();}
        dirty=true;
    }
    void stopLearn(){if(learner.stage()!=midi::PadLearner::Stage::idle&&device.cancel&&deviceOwner)device.cancel(deviceOwner,learner.token());learner.cancel();dirty=true;}
    midi::PadMapping profileMapping(const midi::PadProfile& p)const{midi::PadMapping m;m.enabled=true;m.channel=p.channel;m.input=p.notes;for(unsigned n=0;n<16;++n)m.output[n]=std::uint8_t(padNote(n));return m;}
    bool applyProfile(const midi::PadProfile& profile){
        if(!deviceOwner||!device.apply||!midi::valid(profile))return false;
        if(!device.apply(deviceOwner,profileMapping(profile))){status="DEVICE MAPPING BUSY - TRY AGAIN";dirty=true;return false;}
        applied=profile;selected=profile;status="DEVICE: "+profile.name+" / CH "+std::to_string(profile.channel);dirty=true;return true;
    }
    void updateMapping(){if(applied&&deviceOwner&&device.apply&&!device.apply(deviceOwner,profileMapping(*applied))){status="PAD BANK MAPPING BUSY - TRY AGAIN";dirty=true;}}
    void pollDevice(bool force=false){
        if(!device.poll)return;const auto now=std::chrono::steady_clock::now();if(!force&&now<nextDevicePoll)return;nextDevicePoll=now+std::chrono::milliseconds(50);
        try{const auto view=device.poll();if(!view)return;
            if(deviceOwner!=view->owner){stopLearn();deviceOwner=view->owner;captureSequence=0;applied.reset();defaultPending=true;}
            if(defaultPending){
                defaultPending=view->mapping.enabled?false:!formatKnown;
                if(view->mapping.enabled){for(const auto& p:profiles.profiles)if(p.channel==view->mapping.channel&&p.notes==view->mapping.input){selected=applied=p;break;}if(!applied){midi::PadProfile p{"session","Session device map",view->mapping.channel,view->mapping.input,{}};selected=applied=p;}}
                else if(formatKnown){const auto id=profiles.defaultFor(format);for(const auto& p:profiles.profiles)if(p.id==id&&(p.format.empty()||p.format==format)){applyProfile(p);break;}}
            }
            if(view->overflow&&learner.stage()!=midi::PadLearner::Stage::idle){stopLearn();status="LEARN INPUT OVERFLOW - RETRY WITH ONE PAD AT A TIME";}
            for(const auto& capture:view->captures)if(capture.sequence>captureSequence){captureSequence=capture.sequence;if(capture.token==learner.token()&&learner.stage()!=midi::PadLearner::Stage::idle){learner.feed(capture);if(learner.stage()==midi::PadLearner::Stage::ready&&device.cancel)device.cancel(deviceOwner,learner.token());dirty=true;}}
        }catch(const std::exception& error){status=error.what();dirty=true;}
    }
    void selectPreset(int direction){
        std::vector<const midi::PadProfile*> list;for(const auto& p:profiles.profiles)if(p.format.empty()||p.format==format)list.push_back(&p);
        if(list.empty()){status="NO DEVICE PRESETS FOR "+(format.empty()?std::string("ALL FORMATS"):format);return;}
        auto found=std::find_if(list.begin(),list.end(),[&](const auto* p){return selected&&p->id==selected->id;});const auto index=found==list.end()?(direction>0?-1:0):int(found-list.begin());selected=*list[unsigned((index+int(list.size())+direction)%int(list.size()))];dirty=true;
    }
    void deviceAction(unsigned which){
        release();
        try{
            if(which==deviceButton){stopLearn();showDevicePanel(!devicePanel);entry.reset();if(store)profiles=store->read();}
            else if(which==learnButton){if(mode!=Mode::pads)return;stopLearn();pollDevice(true);if(!deviceOwner||!device.learn){status="WAITING FOR MIDI AUDIO INPUT";return;}const auto token=instrumentToken.fetch_add(1)+1;if(!device.learn(deviceOwner,token)){status="ANOTHER CONTROLLER IS LEARNING - CLOSE IT FIRST";return;}learner.begin(token);showDevicePanel(true);entry.reset();}
            else if(which==learnAll)learner.individual();
            else if(which==learnCancel){stopLearn();showDevicePanel(false);entry.reset();}
            else if(which==learnApply&&learner.stage()==midi::PadLearner::Stage::ready){auto p=learner.profile(midi::newPadProfileId(),"My controller");p.format=format;if(applyProfile(p)){stopLearn();devicePanel=true;status="MAPPING APPLIED - NAME AND SAVE YOUR PRESET";}}
            else if(which==presetPrev||which==presetNext)selectPreset(which==presetNext?1:-1);
            else if(which==presetName&&selected)entry=Entry{presetName,selected->name,true};
            else if(which==presetFormat&&selected){selected->format=selected->format.empty()?format:std::string{};status=selected->format.empty()?"PRESET APPLIES TO ALL FORMATS":"PRESET FORMAT: "+selected->format;}
            else if(which==presetSave&&selected){if(!store)throw std::runtime_error("User preset storage is unavailable");auto p=*selected;p.id=midi::newPadProfileId();store->save(p);profiles=store->read();selected=p;if(applied&&applied->notes==p.notes&&applied->channel==p.channel)applied=p;status="DEVICE PRESET SAVED";}
            else if(which==presetDefault&&selected){if(!store)throw std::runtime_error("User preset storage is unavailable");store->makeDefault(selected->id,selected->format);profiles=store->read();status=selected->format.empty()?"GENERAL DEVICE DEFAULT SAVED":"DEFAULT SAVED FOR "+selected->format;}
            else if(which==presetUse&&selected){if(applyProfile(*selected)){showDevicePanel(false);entry.reset();}}
            else if(which==presetDisable){midi::PadMapping disabled;if(device.apply&&deviceOwner&&device.apply(deviceOwner,disabled)){applied.reset();status="HARDWARE PAD MAPPING OFF";showDevicePanel(false);}else status="DEVICE MAPPING BUSY - TRY AGAIN";}
            else if(which==presetClose){showDevicePanel(false);entry.reset();}
        }catch(const std::exception& error){status=error.what();}dirty=true;
    }
    static std::atomic<std::uint64_t> instrumentToken;
    Impl(){computer.fill(-1);contacts.reserve(16);}
    Rect maximizedBox()const{return parent.inset(std::min(8.,std::max(0.,std::min(parent.width,parent.height)/20)));}
    Rect clamped(Rect r,Mode selected)const{
        const auto area=parent.inset(std::min(6.,std::max(0.,std::min(parent.width,parent.height)/20)));
        r.width=std::clamp(r.width,std::min(360.,area.width),area.width);r.height=std::clamp(r.height,std::min(selected==Mode::pads&&!devicePanel?440.:200.,area.height),area.height);
        // Leave at least 64 px of draggable title, excluding the chrome at
        // the right. The body may extend beyond the editor in either mode.
        r.x=std::clamp(r.x,area.x+136-r.width,area.x+area.width-67);
        r.y=std::clamp(r.y,area.y,area.y+area.height-28);return r;
    }
    Rect dock()const{return {parent.x+6,parent.y+parent.height-34,std::min(300.,parent.width-12),28};}
    void unminimize(){if(!open||!minimized)return;minimized=false;focused=dirty=true;controls.clear();hits.clear();}
    Rect clamped(Rect r)const{return clamped(r,mode);}
    Rect reflow(Rect r,Mode selected)const{
        r=clamped(r,selected);const auto area=parent.inset(std::min(6.,std::max(0.,std::min(parent.width,parent.height)/20)));
        r.x=std::clamp(r.x,area.x,area.x+area.width-r.width);r.y=std::clamp(r.y,area.y,area.y+area.height-r.height);return r;
    }
    Rect defaultBox(Mode selected,Point centre)const{const double width=selected==Mode::pads?560.:680.,height=selected==Mode::pads?628.:260.;return reflow({centre.x-width/2,centre.y-height/2,width,height},selected);}
    void release(){
        if(hooks.releaseAll)hooks.releaseAll();held.fill(0);computer.fill(-1);accessibleHeld.fill(false);contacts.clear();dirty=true;
    }
    bool note(int pitch,bool down){
        if(pitch<0||pitch>127)return false;
        if(down){
            if(held[unsigned(pitch)]++){dirty=true;return true;}
            const MidiEvent event{0,{std::uint8_t(0x90|(channel-1)),std::uint8_t(pitch),std::uint8_t(velocity)},3};
            if(!hooks.send||!hooks.send(event)){release();status="INPUT BUSY — NOTES RELEASED";return false;}
        }else if(held[unsigned(pitch)]&&!--held[unsigned(pitch)]){
            if(hooks.send&&!hooks.send({0,{std::uint8_t(0x80|(channel-1)),std::uint8_t(pitch),0},3})){release();status="INPUT BUSY — NOTES RELEASED";return false;}
        }
        dirty=true;return true;
    }
    void close(){if(!open)return;stopLearn();release();if(hooks.closed)hooks.closed();hooks={};device={};store.reset();profiles={};selected.reset();applied.reset();deviceOwner=0;devicePanel=false;deviceRestore.reset();format.clear();formatKnown=false;open=false;focused=false;drag.reset();press.reset();entry.reset();controls.clear();hits.clear();dirty=true;}
    std::optional<Hit> at(Point point)const{for(auto i=hits.rbegin();i!=hits.rend();++i)if(i->box.contains(point))return *i;return {};}
    int padNote(unsigned n)const{return layoutKnown&&layoutBank==padBank?int(padInfo[n].note):int((36+padBank*16+n)%128);}
    void add(Rect r,unsigned button,std::string name,int pitch=-1,std::uint64_t customIdentity=0){
        Hit hit{r,button,pitch,customIdentity?customIdentity:identity+(pitch>=0?256+unsigned(pitch):button)};hits.push_back(hit);ControlInfo info;info.identity=hit.id;info.bounds=r;info.role=ControlInfo::Role::button;info.label=info.accessibleName=std::move(name);if(pitch>=0){info.hasValue=true;info.value=held[unsigned(pitch)]?1:0;info.minimum=0;info.maximum=1;info.valueText=info.value?"Held":"Released";}controls.push_back(std::move(info));
    }
    void button(Canvas& c,Rect r,unsigned which,std::string_view caption,std::string name={}){
        bevel(c,r,press&&press->hit.button==which);label(c,r,caption,10);add(r,which,name.empty()?std::string(caption):std::move(name));
    }
    void centre(){firstWhite=-1;dirty=true;}
    void act(unsigned which){
        if(which==minimizeButton){release();stopLearn();minimized=true;focused=false;drag.reset();entry.reset();controls.clear();hits.clear();dirty=true;return;}
        if(which==restoreButton){unminimize();return;}
        if(which>=deviceButton){deviceAction(which);return;}
        if(devicePanel&&(which==keysButton||which==padsButton)){stopLearn();showDevicePanel(false);entry.reset();}
        if(which==closeButton){close();return;}
        if(which==maximizeButton){release();if(maximized){maximized=false;box=clamped(restore);}else{restore=box;maximized=true;box=maximizedBox();}}
        else if(which==keysButton||which==padsButton){
            const auto next=which==keysButton?Mode::keyboard:Mode::pads;if(next==mode)return;release();
            const auto normal=maximized?restore:box;modeBounds[unsigned(mode)]=normal;mode=next;
            const auto desired=modeBounds[unsigned(mode)].value_or(defaultBox(mode,{normal.x+normal.width/2,normal.y+normal.height/2}));
            restore=clamped(desired);box=maximized?maximizedBox():restore;
            if(hooks.modeChanged&&!hooks.modeChanged(mode==Mode::pads))status="CONTROLLER MODE SAVE IS PENDING";
        }
        else if(which==channelButton||which==velocityButton){release();entry=Entry{which,std::to_string(which==channelButton?channel:velocity),true};}
        else if(which==octaveDown||which==octaveUp){release();const int direction=which==octaveUp?1:-1;if(mode==Mode::keyboard){computerBase=std::clamp(computerBase+direction*12,0,108);centre();}else{padBank=unsigned(std::clamp(int(padBank)+direction,0,7));updateMapping();}}
        else if(which==centreButton){release();computerBase=60;padBank=0;updateMapping();centre();}
        else if(which==panicButton)release();dirty=true;
    }
    int computerNote(int key)const{
        key=lower(key);const std::string_view mapping=mode==Mode::keyboard?"awsedftgyhujkolp;":"zxcvasdfqwer1234";
        const auto index=mapping.find(char(key));if(key<0||key>127||index==mapping.npos)return -1;
        return mode==Mode::keyboard?computerBase+int(index):padNote(unsigned(index));
    }
    void piano(Canvas& c){
        std::array<int,75> notes{};unsigned total=0;for(int n=0;n<128;++n)if(!darkKey(n))notes[total++]=n;
        const auto count=std::clamp(unsigned(keysBox.width/28),7u,total);const double width=keysBox.width/count;
        if(firstWhite<0){const auto middle=std::lower_bound(notes.begin(),notes.begin()+total,computerBase)-notes.begin();firstWhite=int(middle)-int(count/2);}firstWhite=std::clamp(firstWhite,0,int(total-count));
        auto paintNote=[&](int pitch,Rect r,bool dark){c.fill(r,held[unsigned(pitch)]?active:dark?black:white);c.strokeRounded(r.inset(.5),0,shadow,1);const auto ink=dark&&!held[unsigned(pitch)]?white:black;
            if(pitch%12==0)label(c,{r.x,r.y+r.height-20,r.width,18},noteName(pitch),9,ink);
            const std::string_view keys="awsedftgyhujkolp;";if(pitch>=computerBase&&unsigned(pitch-computerBase)<keys.size()){const char key=char(std::toupper(keys[unsigned(pitch-computerBase)]));label(c,{r.x,r.y+r.height-(pitch%12==0?39:22),r.width,18},std::string(1,key),10,ink);}
            add(r,0,"Note "+noteName(pitch),pitch);
        };
        for(unsigned n=0;n<count;++n)paintNote(notes[unsigned(firstWhite)+n],{keysBox.x+n*width,keysBox.y,width,keysBox.height},false);
        for(unsigned n=0;n+1<count;++n){const int pitch=notes[unsigned(firstWhite)+n]+1;if(darkKey(pitch))paintNote(pitch,{keysBox.x+(n+1)*width-width*.31,keysBox.y,width*.62,keysBox.height*.62},true);}
    }
    void pads(Canvas& c){
        constexpr std::string_view mapping="zxcvasdfqwer1234";
        constexpr std::array<std::uint32_t,4> rims{0xffdea267,0xff8ebe86,0xff78a4cc,0xffb29acf};
        c.gradient(keysBox,Color(0xff303236),Color(0xff202225),5);c.strokeRounded(keysBox.inset(.5),5,Color(0xff111315));
        const auto area=keysBox.inset(std::min(14.,std::min(keysBox.width,keysBox.height)*.05));
        const double gap=std::clamp(std::min(area.width,area.height)*.022,6.,14.);
        const double side=std::floor(std::max(0.,(std::min(area.width,area.height)-gap*3)/4)),size=side*4+gap*3;
        if(side<1)return;const Point origin{area.x+(area.width-size)/2,area.y+(area.height-size)/2};
        for(unsigned n=0;n<16;++n){
            const auto pitch=unsigned(padNote(n));const bool down=held[pitch]!=0;const Rect r{origin.x+(n%4)*(side+gap),origin.y+(3-n/4)*(side+gap),side,side};
            const double radius=std::clamp(side*.07,3.,9.);const auto rim=rims[n/4];
            c.rounded({r.x+1,r.y+3,r.width,r.height},radius,Color(0x99000000));
            c.rounded(r,radius,Color(0xff111315));c.strokeRounded(r.inset(1),radius,Color(down?rim:(0x85000000u|(rim&0xffffff))),down?2.5:1.5);
            Rect rubber=r.inset(std::max(3.,side*.035));if(down)rubber.y+=1.5;
            c.gradient(rubber,down?Color(0xff515255):Color(0xff7b7b7c),down?Color(0xff383a3e):Color(0xff515256),std::max(2.,radius-2));
            c.strokeRounded(rubber.inset(.5),std::max(2.,radius-2),down?Color(0xff303236):Color(0xff99999a),.8);
            // A fixed number of grain marks keeps the rubber texture stable
            // and the drawing cost bounded at any editor scale.
            c.save();c.clip(rubber.inset(2));for(unsigned grain=0;grain<24;++grain){const double x=rubber.x+2+std::fmod(double(grain*37+n*11),std::max(1.,rubber.width-4)),y=rubber.y+2+std::fmod(double(grain*19+n*7),std::max(1.,rubber.height-4));c.fill({x,y,1,1},Color(grain%2?0x17000000:0x12ffffff));}c.restore();
            if(down){c.strokeRounded(rubber.inset(1.5),std::max(2.,radius-3),Color(((128+velocity)<<24)|(rim&0xffffff)),1.5);c.rounded({rubber.x+rubber.width*.3,rubber.y+rubber.height-7,rubber.width*.4,2.5},1,Color(rim));}
            const double offset=down?1.5:0.;const auto lettering=down?Color(0xfff8f7f3):Color(0xffe0e0dc);
            const bool supplied=layoutKnown&&layoutBank==padBank;const auto& info=padInfo[n];
            const std::string sample=supplied?(info.sample.empty()?"EMPTY":info.sample+(info.layers>1?" +"+std::to_string(info.layers-1):"")):"NOTE "+noteName(int(pitch));
            label(c,{r.x+5,r.y+side*.08+offset,side-10,side*.24},(n<9?"0":"")+std::to_string(n+1),std::clamp(side*.12,9.,15.),lettering);
            label(c,{r.x+6,r.y+side*.34+offset,side-12,side*.3},sample,std::clamp(side*.10,9.,13.),lettering);
            label(c,{r.x+5,r.y+side*.72+offset,side-10,side*.17},noteName(int(pitch))+"  ["+std::string(1,char(std::toupper(mapping[n])))+"]",std::clamp(side*.085,8.,11.),lettering);
            add(r,0,"Pad "+std::to_string(n+1)+" / "+noteName(int(pitch))+(supplied?" / "+sample:""),int(pitch),identity+512+padBank*16+n);
        }
    }
    void paintDevicePanel(Canvas& c){
        bevel(c,keysBox);const double x=keysBox.x+8,w=keysBox.width-16,y=keysBox.y+5;const bool learning=learner.stage()!=midi::PadLearner::Stage::idle;
        const double h=std::clamp((keysBox.height-10)/5,1.,24.);
        if(learning){
            label(c,{x,y,w,h},learner.message(),12,navy);label(c,{x,y+h,w,h},"Consecutive notes: pad 1, then pad 16. Custom layout: learn all 16.",9);
            label(c,{x,y+h*2,w,h},"Use physical MIDI input; on-screen keys do not enter the learn wizard.",9);
            const double unit=w/3;button(c,{x,y+h*3,unit-4,h},learnAll,"LEARN ALL 16");if(learner.stage()==midi::PadLearner::Stage::ready)button(c,{x+unit,y+h*3,unit-4,h},learnApply,"APPLY MAPPING");button(c,{x+unit*2,y+h*3,unit-4,h},learnCancel,"CANCEL LEARN");return;
        }
        label(c,{x,y,w,h},"DEVICE PRESETS / "+(format.empty()?std::string("ALL FORMATS"):format),11,navy);
        const double unit=w/4;button(c,{x,y+h,unit-4,h},presetPrev,"< PRESET");button(c,{x+unit,y+h,unit*2-4,h},presetName,entry&&entry->button==presetName?entry->text:selected?selected->name:"LEARN A DEVICE FIRST","Device preset name");button(c,{x+unit*3,y+h,unit-4,h},presetNext,"PRESET >");
        button(c,{x,y+h*2,unit*2-4,h},presetFormat,selected&& !selected->format.empty()?selected->format:"ALL FORMATS","Device preset file extension");button(c,{x+unit*2,y+h*2,unit*2-4,h},presetUse,"USE PRESET");
        button(c,{x,y+h*3,unit*2-4,h},presetSave,"SAVE NEW PRESET");button(c,{x+unit*2,y+h*3,unit*2-4,h},presetDefault,"SET USER DEFAULT");
        button(c,{x,y+h*4,unit*2-4,h},presetDisable,"MAPPING OFF");button(c,{x+unit*2,y+h*4,unit*2-4,h},presetClose,"CLOSE PRESETS");
    }
    void paint(Canvas& c,Rect bounds){
        if(!open)return;
        if(!std::isfinite(bounds.x)||!std::isfinite(bounds.y)||!std::isfinite(bounds.width)||!std::isfinite(bounds.height)||bounds.width<180||bounds.height<140){release();controls.clear();hits.clear();return;}
        if(parent.x!=bounds.x||parent.y!=bounds.y||parent.width!=bounds.width||parent.height!=bounds.height){release();drag.reset();press.reset();parent=bounds;restore=reflow(restore,mode);for(unsigned i=0;i<modeBounds.size();++i)if(modeBounds[i])*modeBounds[i]=reflow(*modeBounds[i],i?Mode::pads:Mode::keyboard);box=maximized?maximizedBox():reflow(box,mode);}
        if(minimized){controls.clear();hits.clear();const auto r=dock();bevel(c,r);c.gradient(r.inset(3),navy,blue);label(c,r.inset(4),title,11,white);add(r,restoreButton,"Restore minimized controller window");dirty=false;return;}
        controls.clear();hits.clear();c.fill({box.x+4,box.y+4,box.width,box.height},Color(0x66000000));bevel(c,box);
        titleBox={box.x+3,box.y+3,box.width-6,24};c.gradient(titleBox,focused?navy:shadow,focused?blue:face);auto caption=title;if(mode==Mode::pads){const auto marker=caption.find("Keyboard_");if(marker!=std::string::npos)caption.replace(marker,9,"Drum_Pads_");}label(c,{titleBox.x+6,titleBox.y,titleBox.width-79,titleBox.height},caption,12,white);
        const Rect close{titleBox.x+titleBox.width-21,titleBox.y+3,18,18},maximize{close.x-21,close.y,18,18};button(c,maximize,maximizeButton,"",maximized?"Restore controller window":"Maximize controller window");button(c,close,closeButton,"","Close controller window");
        const Rect minimize{maximize.x-21,maximize.y,18,18};button(c,minimize,minimizeButton,"","Minimize controller window");c.line({minimize.x+4,minimize.y+13},{minimize.x+12,minimize.y+13},black,2);
        c.line({close.x+5,close.y+5},{close.x+13,close.y+13},black,1.5);c.line({close.x+13,close.y+5},{close.x+5,close.y+13},black,1.5);
        if(maximized){c.strokeRounded({maximize.x+6,maximize.y+4,8,7},0,black);c.fill({maximize.x+4,maximize.y+7,8,7},face);c.strokeRounded({maximize.x+4,maximize.y+7,8,7},0,black);c.line({maximize.x+4,maximize.y+8},{maximize.x+12,maximize.y+8},black,2);}else{c.strokeRounded(maximize.inset(4),0,black);c.line({maximize.x+4,maximize.y+5},{maximize.x+14,maximize.y+5},black,2);}
        const double y=box.y+34;const double unit=(box.width-24)/9;
        button(c,{box.x+8,y,unit-3,25},keysButton,"KEYS");button(c,{box.x+8+unit,y,unit-3,25},padsButton,"PADS");
        button(c,{box.x+8+unit*2,y,unit-3,25},channelButton,entry&&entry->button==channelButton?entry->text:"CH "+std::to_string(channel),"MIDI channel");
        button(c,{box.x+8+unit*3,y,unit-3,25},velocityButton,entry&&entry->button==velocityButton?entry->text:"VEL "+std::to_string(velocity),"Note velocity");
        button(c,{box.x+8+unit*4,y,unit-3,25},octaveDown,"<",mode==Mode::keyboard?"Computer keyboard octave down":"Previous pad bank");
        button(c,{box.x+8+unit*5,y,unit-3,25},octaveUp,">",mode==Mode::keyboard?"Computer keyboard octave up":"Next pad bank");
        button(c,{box.x+8+unit*6,y,unit*2-3,25},centreButton,mode==Mode::keyboard?"CENTER C4":"RESET PADS",mode==Mode::keyboard?"Recenter on middle C":"Reset pad bank");button(c,{box.x+8+unit*8,y,unit-3,25},panicButton,"PANIC");
        const bool hasDevice=bool(device.poll)&&mode==Mode::pads;const double extra=hasDevice?30:0;
        if(hasDevice){const double width=box.width-24;button(c,{box.x+8,box.y+64,width*.72-4,25},deviceButton,"DEVICE: "+(applied?applied->name:"UNMAPPED")+(format.empty()?"":" / "+format),"Device presets");button(c,{box.x+8+width*.72,box.y+64,width*.28,25},learnButton,"MIDI LEARN","Learn hardware pads");}
        keysBox={box.x+8,box.y+67+extra,box.width-16,box.height-94-extra};if(devicePanel)paintDevicePanel(c);else if(mode==Mode::keyboard)piano(c);else pads(c);
        label(c,{box.x+8,box.y+box.height-23,box.width-30,18},status.empty()?(focused?(mode==Mode::keyboard?"A W S E D F… play  |  Z / X octave  |  Space stays with host":"Z X C V / A S D F / Q W E R / 1 2 3 4 play pads"):"Click this window to play; Escape releases and returns keyboard focus"):status,9);
        grip={box.x+box.width-17,box.y+box.height-17,15,15};if(!maximized)for(unsigned n=0;n<3;++n)c.line({grip.x+3+n*4,grip.y+grip.height-2},{grip.x+grip.width-2,grip.y+3+n*4},shadow);
        if(focused)for(const auto& control:controls)if(control.identity==focusId)c.strokeRounded(control.bounds.inset(2),0,blue,1.5);
        dirty=false;
    }
    bool pointer(const PointerEvent& event){
        if(!open)return false;
        if(!std::isfinite(event.position.x)||!std::isfinite(event.position.y)||!std::isfinite(event.wheel))return false;
        if(event.action==PointerAction::cancel){release();drag.reset();press.reset();entry.reset();focused=false;return true;}
        if(drag){if(event.identity!=drag->pointer)return true;if(event.action==PointerAction::move){auto next=drag->original;const double x=event.position.x-drag->start.x,y=event.position.y-drag->start.y;if(drag->resize){next.width+=x;next.height+=y;}else{next.x+=x;next.y+=y;}box=clamped(next);dirty=true;}else if(event.action==PointerAction::up)drag.reset();return true;}
        if(press){if(event.identity!=press->pointer)return true;if(event.action==PointerAction::up){const auto button=press->hit;press.reset();if(button.box.contains(event.position))act(button.button);dirty=true;}return true;}
        if(minimized){if(event.action==PointerAction::down&&dock().contains(event.position)){press=Press{event.identity,Hit{dock(),restoreButton,-1,identity+restoreButton}};return true;}return dock().contains(event.position);}
        for(auto& contact:contacts)if(contact.pointer==event.identity){
            if(event.action==PointerAction::up){const auto pitch=contact.note;std::erase_if(contacts,[&](const auto& item){return item.pointer==event.identity;});note(pitch,false);}
            else if(event.action==PointerAction::move){const auto hit=at(event.position);const int pitch=hit?hit->note:-1;if(pitch!=contact.note){const auto previous=contact.note;contact.note=pitch;note(previous,false);note(pitch,true);}}return true;
        }
        if(event.action==PointerAction::down){
            if(!box.contains(event.position)){if(focused){release();focused=false;entry.reset();}return false;}
            focused=true;status.clear();
            if(!maximized&&grip.contains(event.position)){release();drag=Drag{event.identity,event.position,box,true};return true;}
            if(const auto hit=at(event.position)){focusId=hit->id;if(hit->note>=0){if(contacts.size()<16){contacts.push_back({event.identity,hit->note});note(hit->note,true);}}else press=Press{event.identity,*hit};dirty=true;return true;}
            if(titleBox.contains(event.position)&&!maximized){release();drag=Drag{event.identity,event.position,box,false};return true;}
            return true;
        }
        if(event.action==PointerAction::wheel&&box.contains(event.position)){if(mode==Mode::keyboard){firstWhite=std::max(0,firstWhite-int(std::round(std::clamp(event.wheel,-25.,25.)*3)));dirty=true;}return true;}
        return box.contains(event.position);
    }
    bool keyboard(const KeyEvent& event){
        if(!open||minimized||!focused)return false;const int key=lower(event.key);
        if(entry){
            if((event.control||event.command)&&key=='a'){entry->replace=true;dirty=true;return true;}
            if(key==27){entry.reset();dirty=true;return true;}
            if(key==13){if(entry->button==presetName){if(selected&&!entry->text.empty()){selected->name=entry->text;entry.reset();status.clear();}else status="ENTER A DEVICE PRESET NAME";dirty=true;return true;}unsigned number{};const auto parsed=std::from_chars(entry->text.data(),entry->text.data()+entry->text.size(),number);const unsigned maximum=entry->button==channelButton?16:127;if(parsed.ec==std::errc{}&&parsed.ptr==entry->text.data()+entry->text.size()&&number>=1&&number<=maximum){if(entry->button==channelButton)channel=number;else velocity=number;entry.reset();status.clear();}else status="ENTER A WHOLE NUMBER IN RANGE";dirty=true;return true;}
            if(key==8){if(entry->replace){entry->text.clear();entry->replace=false;}else if(!entry->text.empty()){auto size=entry->text.size()-1;while(size&&(std::uint8_t(entry->text[size])&0xc0)==0x80)--size;entry->text.resize(size);}dirty=true;return true;}
            if(entry->button==presetName&&key>=32&&key<=126&&!event.control&&!event.alt&&!event.command){if(entry->replace){entry->text.clear();entry->replace=false;}if(entry->text.size()<128){const char value=char(event.shift?event.key:key);entry->text+=value;}dirty=true;return true;}
            if(key>='0'&&key<='9'&&!event.control&&!event.alt&&!event.command){if(entry->replace){entry->text.clear();entry->replace=false;}if(entry->text.size()<3)entry->text+=char(key);dirty=true;return true;}return key!=32&&!event.control&&!event.alt&&!event.command;
        }
        if(event.control||event.alt||event.command)return false;
        if(devicePanel&&key!=9&&key!=13){if(key==27){stopLearn();showDevicePanel(false);entry.reset();dirty=true;return true;}return key!=32;}
        if(key==9){
            std::vector<std::uint64_t> chrome;for(const auto& hit:hits)if(hit.button)chrome.push_back(hit.id);
            if(!chrome.empty()){const auto found=std::find(chrome.begin(),chrome.end(),focusId);const auto at=found==chrome.end()?std::size_t(event.shift?0:chrome.size()-1):std::size_t(found-chrome.begin());focusId=chrome[(at+chrome.size()+(event.shift?-1:1))%chrome.size()];dirty=true;}return true;
        }
        if(key==13&&focusId){return accessibleAction(focusId,AccessibleAction::activate,0);}
        if(key==27){release();focused=false;return true;}
        if(key==keys::left||(mode==Mode::keyboard&&key=='z')){if(!event.repeat)act(octaveDown);return true;}
        if(key==keys::right||(mode==Mode::keyboard&&key=='x')){if(!event.repeat)act(octaveUp);return true;}
        if(key==keys::home){act(centreButton);return true;}
        const int pitch=computerNote(key);if(pitch<0||pitch>127||key<0||key>=int(computer.size()))return false;
        if(computer[unsigned(key)]<0){computer[unsigned(key)]=pitch;note(pitch,true);}return true;
    }
    bool keyReleased(const KeyEvent& event){const int key=lower(event.key);if(key<0||key>=int(computer.size())||computer[unsigned(key)]<0)return false;const int pitch=computer[unsigned(key)];computer[unsigned(key)]=-1;note(pitch,false);return true;}
    bool accessibleAction(std::uint64_t id,AccessibleAction action,double value){
        if(!open)return false;const auto hit=std::find_if(hits.begin(),hits.end(),[&](const auto& item){return item.id==id;});if(hit==hits.end())return false;focused=true;
        focusId=id;dirty=true;if(action==AccessibleAction::focus)return true;
        if(action==AccessibleAction::activate){if(hit->note>=0){const auto index=hit->id>=identity+512?128+unsigned(hit->id-identity-512):unsigned(hit->note);auto& held=accessibleHeld[index];held=!held;return note(hit->note,held);}act(hit->button);return true;}
        if(hit->button==channelButton||hit->button==velocityButton){const unsigned maximum=hit->button==channelButton?16:127;auto& selected=hit->button==channelButton?channel:velocity;const double next=action==AccessibleAction::setValue?value:double(selected)+(action==AccessibleAction::increment?1:-1);if(!std::isfinite(next)||next<1||next>maximum||next!=std::floor(next))return false;release();selected=unsigned(next);return true;}return false;
    }
};
std::atomic<std::uint64_t> InstrumentController::Impl::instrumentToken{};
InstrumentController::InstrumentController():impl_(std::make_unique<Impl>()){}
InstrumentController::~InstrumentController(){try{close();}catch(...){}}
void InstrumentController::open(std::string title,Hooks hooks,Rect parent,Mode mode){
    impl_->close();if(!hooks.send||!std::isfinite(parent.x)||!std::isfinite(parent.y)||!std::isfinite(parent.width)||!std::isfinite(parent.height)||parent.width<100||parent.height<100)throw std::invalid_argument("Invalid instrument controller parent/hooks");auto& p=*impl_;p.hooks=std::move(hooks);p.title=std::move(title);p.parent=parent;p.mode=mode;p.modeBounds={};p.layoutKnown=false;p.padBank=0;p.box=p.defaultBox(mode,{parent.x+parent.width/2,parent.y+parent.height*.6});p.restore=p.box;p.open=p.focused=p.dirty=true;p.maximized=p.minimized=false;p.firstWhite=-1;p.focusId=mode==Mode::pads?identity+512:identity+256+unsigned(p.computerBase);p.status.clear();
}
void InstrumentController::close(){impl_->close();}
bool InstrumentController::isOpen()const noexcept{return impl_->open;}
bool InstrumentController::isMaximized()const noexcept{return impl_->maximized;}
bool InstrumentController::isMinimized()const noexcept{return impl_->open&&impl_->minimized;}
void InstrumentController::restore(){impl_->unminimize();}
bool InstrumentController::hasKeyboardFocus()const noexcept{return impl_->open&&impl_->focused;}
unsigned InstrumentController::padBank()const noexcept{return impl_->padBank;}
bool InstrumentController::padLayout(unsigned bank,std::span<const PadInfo> pads){
    if(bank>7||pads.size()!=16)throw std::invalid_argument("Invalid controller pad bank");std::array<PadInfo,16> next;
    for(unsigned n=0;n<16;++n){const auto& value=pads[n];if(value.note>127||value.name.size()>512||value.sample.size()>1024||value.layers>4096||value.name.find('\0')!=value.name.npos||value.sample.find('\0')!=value.sample.npos)throw std::invalid_argument("Invalid controller pad labels");next[n]=value;}
    auto& p=*impl_;if(p.layoutKnown&&p.layoutBank==bank&&p.padInfo==next)return false;
    if(bank==p.padBank)for(unsigned n=0;n<16;++n)if(p.padNote(n)!=int(next[n].note)){p.release();break;}
    p.padInfo=std::move(next);p.layoutBank=bank;p.layoutKnown=p.dirty=true;p.updateMapping();return true;
}
void InstrumentController::clearPadLayout(){auto& p=*impl_;if(!p.layoutKnown)return;for(unsigned n=0;n<16;++n)if(p.padNote(n)!=int((36+p.padBank*16+n)%128)){p.release();break;}p.layoutKnown=false;p.dirty=true;p.updateMapping();}
void InstrumentController::devices(DeviceHooks hooks){auto& p=*impl_;p.stopLearn();p.device=std::move(hooks);p.deviceOwner=0;p.defaultPending=true;p.nextDevicePoll={};try{const auto path=p.device.profilesFile.empty()?midi::PadProfileStore::userFile():p.device.profilesFile;p.store=std::make_unique<midi::PadProfileStore>(path);p.profiles=p.store->read();}catch(const std::exception& error){p.store.reset();p.profiles={};p.status=error.what();}p.pollDevice(true);p.dirty=true;}
void InstrumentController::libraryFormat(std::string_view extension){auto& p=*impl_;const auto format=midi::padFormat(extension);if(format==p.format&&p.formatKnown)return;p.format=format;p.formatKnown=true;if(!p.applied)p.defaultPending=true;p.nextDevicePoll={};p.dirty=true;}
std::uint64_t InstrumentController::focusedControl()const noexcept{return impl_->focused?impl_->focusId:0;}
Rect InstrumentController::bounds()const noexcept{return impl_->minimized?impl_->dock():impl_->box;}
void InstrumentController::paint(Canvas& canvas,Rect parent){impl_->paint(canvas,parent);}
bool InstrumentController::pointer(const PointerEvent& event){return impl_->pointer(event);}
bool InstrumentController::keyboard(const KeyEvent& event){return impl_->keyboard(event);}
bool InstrumentController::keyReleased(const KeyEvent& event){return impl_->keyReleased(event);}
void InstrumentController::cancelInput(){if(impl_->open){impl_->release();impl_->drag.reset();impl_->press.reset();impl_->entry.reset();impl_->focused=false;}}
bool InstrumentController::tick(){if(impl_->open)impl_->pollDevice();return std::exchange(impl_->dirty,false);}
std::span<const ControlInfo> InstrumentController::controls()const noexcept{return impl_->controls;}
bool InstrumentController::accessibleAction(std::uint64_t id,AccessibleAction action,double value){return impl_->accessibleAction(id,action,value);}
}
