#include <cell/instrument_controller.hpp>
#include <cell/midi_control.hpp>
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>

using namespace cell;
using namespace cell::ui;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Point centre(Rect r){return {r.x+r.width/2,r.y+r.height/2};}
struct Fixture {
    midi::ControlInput input;std::uint64_t sequence=1;unsigned releases{},closed{};InstrumentController controller;Surface surface;Rect parent{0,0,1280,920};
    Fixture(){check(input.submit({1,sequence,midi::ControlAction::open,{}}),"Controller fixture lease failed");controller.open("MacroHard '98 Keyboard_Sampler",{
        [&](MidiEvent e){return input.submit({1,++sequence,midi::ControlAction::event,e});},
        [&]{++releases;input.submit({1,++sequence,midi::ControlAction::releaseAll,{}});},
        [&]{++closed;input.submit({1,++sequence,midi::ControlAction::close,{}});}},parent);paint();}
    void paint(){surface.resize(int(parent.width),int(parent.height));surface.render([&](Canvas& c){c.fill(parent,Color(0xff0c1720));controller.paint(c,parent);});}
    ControlInfo find(std::string_view name){for(const auto& info:controller.controls())if(info.label==name)return info;throw std::runtime_error("Missing controller face: "+std::string(name));}
    void activate(std::string_view name){check(controller.accessibleAction(find(name).identity,AccessibleAction::activate),"Controller chrome action rejected");paint();}
    void bounds(){std::set<std::uint64_t> ids;const auto box=controller.bounds();for(const auto& info:controller.controls())check(ids.insert(info.identity).second&&info.bounds.x>=box.x&&info.bounds.y>=box.y&&info.bounds.width>0&&info.bounds.height>0&&info.bounds.x+info.bounds.width<=box.x+box.width+.01&&info.bounds.y+info.bounds.height<=box.y+box.height+.01,"Controller face escaped its window or duplicated identity");}
};
void keyboard(){
    Fixture f;f.bounds();check(f.controller.keyboard({'a'}),"Computer keyboard note rejected");auto events=f.input.drain();check(events.size()==1&&events[0].data[1]==60,"Computer keyboard A did not play middle C");
    KeyEvent repeat{'a'};repeat.repeat=true;check(f.controller.keyboard(repeat)&&f.input.drain().empty(),"Auto-repeat retriggered a held key");
    KeyEvent release{'A'};release.control=true;check(f.controller.keyReleased(release),"Modified key-up lost a held note");events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x80&&events[0].data[1]==60,"Key-up did not release its original note");
    check(!f.controller.keyboard({32})&&!f.controller.keyboard({'s',false,true}),"Controller stole host transport or modified shortcuts");
    check(f.controller.keyboard({'z'})&&f.controller.keyboard({'a'}),"Keyboard octave change rejected");events=f.input.drain();check(events.size()==1&&events[0].data[1]==48,"Keyboard octave change did not affect subsequent notes");f.controller.keyReleased({'a'});f.input.drain();
    f.activate("Recenter on middle C");check(f.controller.keyboard({'a'}),"Recentered keyboard rejected a note");f.input.drain();check(f.controller.keyboard({27}),"Escape did not leave controller keyboard focus");events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x80&&!f.controller.keyboard({'a'}),"Escape retained a note or stole unfocused shortcuts");
    f.activate("MIDI channel");for(char ch:std::string("16"))f.controller.keyboard({ch});f.controller.keyboard({13});f.paint();check(f.controller.keyboard({'a'}),"Channel entry rejected");events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x9f,"Whole-number channel entry did not route MIDI");
    f.controller.cancelInput();events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x8f,"Focus loss left a controller note held");
}
void pointerAndChrome(const std::filesystem::path& output){
    Fixture f;const auto original=f.controller.bounds();const auto note=f.find("Note C4");const auto at=Point{note.bounds.x+note.bounds.width/2,note.bounds.y+note.bounds.height-10};const PointerIdentity first{1,PointerKind::touch},second{2,PointerKind::touch};
    f.controller.pointer({PointerAction::down,at,0,false,false,first});f.controller.pointer({PointerAction::down,at,0,false,false,second});auto events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x90,"Two contacts on one key retriggered independently");
    f.controller.pointer({PointerAction::up,{-10,-10},0,false,false,first});check(f.input.drain().empty(),"One contact released another contact's note");f.controller.pointer({PointerAction::up,{-10,-10},0,false,false,second});events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x80,"Pointer release outside controller left a note held");
    const Point title{original.x+50,original.y+14};f.controller.pointer({PointerAction::down,title,0,false,false,first});f.controller.pointer({PointerAction::move,{title.x+70,title.y+40},0,false,false,second});check(f.controller.bounds().x==original.x,"Second contact stole controller window drag");f.controller.pointer({PointerAction::move,{title.x+70,title.y+40},0,false,false,first});f.controller.pointer({PointerAction::up,{title.x+70,title.y+40},0,false,false,first});check(f.controller.bounds().x!=original.x,"Controller title bar did not drag");f.paint();f.bounds();
    const auto dragged=f.controller.bounds();const Point grip{dragged.x+dragged.width-5,dragged.y+dragged.height-5};f.controller.pointer({PointerAction::down,grip});f.controller.pointer({PointerAction::move,{grip.x+80,grip.y+90}});f.controller.pointer({PointerAction::up,{grip.x+80,grip.y+90}});f.paint();check(f.controller.bounds().width>dragged.width&&f.controller.bounds().height>dragged.height,"Controller resize grip did not resize");const auto normal=f.controller.bounds();
    f.activate("Maximize controller window");check(f.controller.isMaximized(),"Controller maximize failed");f.bounds();f.activate("Restore controller window");check(!f.controller.isMaximized()&&f.controller.bounds().width==normal.width&&f.controller.bounds().height==normal.height,"Controller restore lost the user's normal size");
    f.activate("PADS");f.bounds();f.controller.keyboard({'z'});events=f.input.drain();check(events.size()==1&&events[0].data[1]==36,"Pad controller did not map Z to pad 1");f.controller.keyReleased({'Z'});f.input.drain();
    if(!output.empty()){std::filesystem::create_directories(output);f.paint();check(f.surface.writePng((output/"classic-pad-controller.png").string()),"Controller preview write failed");f.controller.keyboard({'z'});f.paint();check(f.surface.writePng((output/"mpc-pad-controller-pressed.png").string()),"Pressed pad preview write failed");f.controller.keyReleased({'z'});f.activate("KEYS");check(f.surface.writePng((output/"classic-keyboard-controller.png").string()),"Keyboard preview write failed");}
    f.activate("Close controller window");check(!f.controller.isOpen()&&f.closed==1,"Controller close did not release its target lease");
}
void minimizeAndOffscreen(const std::filesystem::path& output){
    Fixture f;const auto original=f.controller.bounds();
    check(f.controller.keyboard({'a'}),"Minimize fixture refused note");f.input.drain();
    f.activate("Minimize controller window");
    auto events=f.input.drain();check(f.controller.isOpen()&&f.controller.isMinimized()&&!f.controller.hasKeyboardFocus()&&events.size()==1&&events[0].data[0]==0x80,"Minimize retained a note, closed the lease or kept keyboard focus");
    check(!f.controller.keyboard({'a'})&&!f.controller.keyboard({32}),"Minimized controller intercepted host input");
    f.bounds();check(f.controller.controls().size()==1&&f.controller.bounds().y>f.parent.height-40,"Controller did not minimize to the bottom of the editor");
    if(!output.empty()){std::filesystem::create_directories(output);check(f.surface.writePng((output/"classic-controller-minimized.png").string()),"Minimized preview write failed");}
    f.activate("Restore minimized controller window");check(!f.controller.isMinimized()&&f.controller.bounds().width==original.width&&f.controller.bounds().height==original.height,"Dock restore lost window size");
    f.activate("Maximize controller window");f.activate("Minimize controller window");f.activate("Restore minimized controller window");check(f.controller.isMaximized(),"Minimize lost the maximized restore state");f.activate("Restore controller window");
    for(const Point delta:{Point{-4000,4000},Point{4000,-4000}}){
        const auto before=f.controller.bounds();const Point title{before.x+40,before.y+14};
        f.controller.pointer({PointerAction::down,title});f.controller.pointer({PointerAction::move,{title.x+delta.x,title.y+delta.y}});f.controller.pointer({PointerAction::up,{title.x+delta.x,title.y+delta.y}});f.paint();
        const auto box=f.controller.bounds();check((box.x<0||box.x+box.width>f.parent.width)&&box.y>=0&&box.y+28<=f.parent.height,"Controller could not extend offscreen with its title reachable");
        const double left=std::max(6.,box.x+3),right=std::min(f.parent.width-6,box.x+box.width-72);
        check(right-left>=64,"Offscreen controller lost its draggable title segment");
        const Point grab{(left+right)/2,box.y+14},move{grab.x+original.x-box.x,grab.y+original.y-box.y};
        f.controller.pointer({PointerAction::down,grab});f.controller.pointer({PointerAction::move,move});f.controller.pointer({PointerAction::up,move});f.paint();
        check(std::abs(f.controller.bounds().x-original.x)<.01&&std::abs(f.controller.bounds().y-original.y)<.01,"Partially offscreen controller could not be dragged fully back");
    }
}
void viewports(){
    for(const Point size:{Point{620,980},Point{1040,760},Point{2800,2000}}){Fixture f;f.parent.width=size.x;f.parent.height=size.y;f.paint();f.bounds();f.activate("Maximize controller window");f.bounds();f.parent={0,0,620,760};f.paint();f.bounds();f.activate("Restore controller window");f.bounds();}
}
void squarePads(){
    for(const Point size:{Point{620,980},Point{1040,760},Point{2800,2000},Point{980,620}}){
        Fixture f;f.parent.width=size.x;f.parent.height=size.y;f.paint();const auto keyboard=f.controller.bounds();f.activate("PADS");
        auto squares=[&]{f.bounds();unsigned count=0;double side=0;for(const auto& control:f.controller.controls())if(control.label.starts_with("Pad ")){check(std::abs(control.bounds.width-control.bounds.height)<1.e-8,"Pad stretched into a rectangle");if(count)check(control.bounds.width==side,"Pads have inconsistent sizes");side=control.bounds.width;++count;}check(count==16&&side>0,"Controller lost its 4 by 4 square pad grid");};
        squares();const auto normal=f.controller.bounds();f.activate("Maximize controller window");squares();f.activate("Restore controller window");squares();check(f.controller.bounds().width==normal.width&&f.controller.bounds().height==normal.height,"Pad restore lost its normal window size");f.activate("KEYS");check(f.controller.bounds().width==keyboard.width&&f.controller.bounds().height==keyboard.height,"Pad mode lost the separate keyboard window dimensions");f.activate("PADS");squares();
    }
}
void padMetadata(){
    Fixture f;std::array<InstrumentController::PadInfo,16> pads;for(unsigned n=0;n<16;++n)pads[n]={36+n,"Pad "+std::to_string(n+1),{},0};pads[0]={48,"Kick","kick.wav",2};pads[1]={48,"Layered kick","soft.wav",1};f.controller.padLayout(0,pads);f.activate("PADS");f.bounds();
    check(f.find("Pad 1 / C3 / kick.wav +1").hasValue&&f.find("Pad 3 / D2 / EMPTY").hasValue,"Loaded filenames, layers or empty pad labels were lost");check(f.controller.keyboard({'z'}),"Mapped pad key rejected");auto events=f.input.drain();check(events.size()==1&&events[0].data[1]==48,"Imported pad note map did not reach MIDI");pads[0].sample="renamed.wav";f.controller.padLayout(0,pads);check(f.input.drain().empty(),"Refreshing sample labels released an unchanged held pad");f.controller.keyReleased({'z'});f.input.drain();f.paint();
    const auto first=f.find("Pad 1 / C3 / renamed.wav +1"),second=f.find("Pad 2 / C3 / soft.wav");f.controller.accessibleAction(first.identity,AccessibleAction::activate);f.controller.accessibleAction(second.identity,AccessibleAction::activate);events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x90,"Duplicate mapped pads retriggered the same owned note");f.controller.accessibleAction(first.identity,AccessibleAction::activate);check(f.input.drain().empty(),"One mapped pad released another pad's note");f.controller.accessibleAction(second.identity,AccessibleAction::activate);events=f.input.drain();check(events.size()==1&&events[0].data[0]==0x80,"Last mapped pad owner did not release its note");
    pads[0].layers=384;f.controller.padLayout(0,pads);f.paint();check(f.find("Pad 1 / C3 / renamed.wav +383").hasValue,"Controller rejected a complete multisample layer count");pads[0].sample="Échantillon très long de grosse caisse.wav";f.controller.padLayout(0,pads);f.paint();check(f.find("Pad 1 / C3 / Échantillon très long de grosse caisse.wav +383").hasValue,"Truncated pad painting lost the full Unicode asset name");auto invalid=pads;invalid[0].layers=4097;try{f.controller.padLayout(0,invalid);throw std::runtime_error("Unbounded layer labels were accepted");}catch(const std::invalid_argument&){}
    f.activate("Next pad bank");check(f.controller.padBank()==1&&f.controller.keyboard({'z'}),"Pad bank navigation rejected");events=f.input.drain();check(events.size()==1&&events[0].data[1]==52,"Pad bank retained an older bank's note map");f.controller.keyReleased({'z'});f.input.drain();f.activate("Reset pad bank");check(f.controller.padBank()==0,"Pad bank reset failed");
}
void deviceLearn(const std::filesystem::path& output){
    const auto directory=std::filesystem::temp_directory_path()/midi::newPadProfileId();std::filesystem::create_directories(directory);
    try{
        Fixture f;midi::PadInput hardware;InstrumentController::DeviceHooks hooks;
        hooks.profilesFile=directory/"profiles.json";hooks.poll=[&]{return std::optional(hardware.poll());};hooks.learn=[&](auto owner,auto token){return owner==hardware.owner()&&hardware.capture(token);};hooks.cancel=[&](auto owner,auto token){return owner==hardware.owner()&&hardware.cancel(token);};hooks.apply=[&](auto owner,const auto& m){return owner==hardware.owner()&&hardware.mapping(m);};f.controller.devices(hooks);f.controller.libraryFormat(".xpm");f.paint();for(const auto& control:f.controller.controls())check(control.label!="Learn hardware pads","Keyboard mode exposed pad MIDI learn");f.activate("PADS");f.bounds();f.activate("Learn hardware pads");
        std::array<MidiEvent,2> input{{{1,{0x99,60,80},3},{5,{0x99,75,100},3}}};std::array<MidiEvent,32> scratch;std::size_t count{};check(hardware.process(input,scratch,64,count),"UI learn hardware fixture failed");std::this_thread::sleep_for(std::chrono::milliseconds(55));f.controller.tick();f.paint();f.bounds();
        check(f.controller.bounds().height==264,"Learn wizard retained the tall pad-grid window");check(!f.controller.keyboard({32}),"Learn wizard stole the host transport key");f.activate("APPLY MAPPING");check(hardware.mapping().enabled&&hardware.mapping().channel==10&&hardware.mapping().input[0]==60&&hardware.mapping().output[0]==36,"Learn wizard did not apply the hardware profile");
        f.activate("Device preset name");for(char ch:std::string("pads"))f.controller.keyboard({ch});f.controller.keyboard({13});f.paint();f.activate("SAVE NEW PRESET");f.activate("SET USER DEFAULT");const auto saved=midi::PadProfileStore(hooks.profilesFile).read();check(saved.profiles.size()==1&&saved.profiles[0].name=="pads"&&saved.profiles[0].format==".xpm"&&saved.defaultFor(".xpm")==saved.profiles[0].id,"Device name/format/default did not persist");
        if(!output.empty()){std::filesystem::create_directories(output);f.surface.writePng((output/"macrohard-device-presets.png").string());}f.activate("CLOSE PRESETS");check(f.controller.bounds().height==628,"Closing compact presets lost the full pad-grid size");f.activate("Next pad bank");check(hardware.mapping().output[0]==52,"Hardware profile did not follow the active pad bank");
        std::array<InstrumentController::PadInfo,16> pads;for(unsigned n=0;n<16;++n)pads[n]={80+n,"","",0};f.controller.padLayout(1,pads);check(hardware.mapping().output[0]==80,"Imported pad assignment did not update the hardware target");
        f.activate("Learn hardware pads");f.activate("CANCEL LEARN");f.controller.close();midi::PadInput fresh;hooks.poll=[&]{return std::optional(fresh.poll());};hooks.learn=[&](auto owner,auto token){return owner==fresh.owner()&&fresh.capture(token);};hooks.cancel=[&](auto owner,auto token){return owner==fresh.owner()&&fresh.cancel(token);};hooks.apply=[&](auto owner,const auto& m){return owner==fresh.owner()&&fresh.mapping(m);};f.controller.open("MacroHard '98 Keyboard_Sampler",{[](MidiEvent){return true;},{},{}},f.parent);f.controller.devices(hooks);f.controller.libraryFormat("xpm");f.controller.tick();check(fresh.mapping().enabled&&fresh.mapping().input[0]==60&&fresh.mapping().channel==10,"A fresh controller did not load the saved format default");
        f.paint();f.activate("PADS");f.activate("Maximize controller window");const auto maximized=f.controller.bounds();f.activate("Device presets");check(!f.controller.isMaximized()&&f.controller.bounds().height==264,"Device presets failed to compact a maximized pad window");f.activate("CLOSE PRESETS");check(f.controller.isMaximized()&&f.controller.bounds().height==maximized.height,"Closing device presets failed to restore the maximized pads");f.activate("Device presets");f.parent={0,0,380,230};f.paint();f.bounds();f.activate("MAPPING OFF");check(!fresh.mapping().enabled,"User could not disable the learned mapping");
        std::filesystem::remove_all(directory);
    }catch(...){std::filesystem::remove_all(directory);throw;}
}
}
int main(int argc,char** argv){try{const auto output=argc>1?std::filesystem::path(argv[1]):std::filesystem::path{};keyboard();pointerAndChrome(output);minimizeAndOffscreen(output);viewports();squarePads();padMetadata();deviceLearn(output);std::cout<<"PASS floating keyboard/pad controller, square pads, sample labels, hardware learn wizard, named per-extension defaults, imported note maps, held releases and responsive classic chrome\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
