#include <cell/dsp.hpp>
#include <cell/ui_components.hpp>
#include <cell/ui_interaction.hpp>
#include <chrono>
#include <complex>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace cell;
using namespace cell::ui;
using namespace cell::ui::components;
const std::array<Parameter,4> fields{{
 {101,"level","Level","dB",-60,12,0,.01,1},
 {102,"cutoff","Cutoff","Hz",20,20000,5000,1,.3},
 {103,"resonance","Resonance","Q",.1,6,.707,.001,1},
 {104,"mix","Mix","%",0,100,100,.1,1}}};
constexpr std::array<ValueFormat,4> formats{{{ValueUnit::decibels,1},{ValueUnit::hertz,2},{ValueUnit::q,3},{ValueUnit::percent,1}}};
const PluginDescriptor descriptor{"org.cell.examples.controls","Cell Control Gallery","Cell SDK","0.1.0",2,fields,true};
class GalleryProcessor final:public Processor {
public:
 void prepare(const ProcessSpec& specification)override{rate_=specification.sampleRate;gain_.prepare(rate_,.02);mix_.prepare(rate_,.02);reset();}
 void reset()noexcept override{for(auto& filter:filters_)filter.reset();gain_.reset(1);mix_.reset(1);cutoff_=q_=-1;}
 void process(AudioBlock<float> audio,const ParameterBank& values,const Transport&,std::span<const MidiEvent>)noexcept override{
  const auto cutoff=std::min(values.get(102),rate_*.45),q=values.get(103);
  if(cutoff!=cutoff_||q!=q_){const auto coefficients=dsp::BiquadCoefficients::lowpass(rate_,cutoff,q);for(auto& filter:filters_)filter.coefficients(coefficients);cutoff_=cutoff;q_=q;}
  gain_.target(dbToGain(float(values.get(101))));mix_.target(float(values.get(104)*.01));
  for(unsigned i=0;i<audio.frames;++i){const float gain=gain_.next(),mix=mix_.next();for(unsigned c=0;c<audio.channelCount;++c){auto& x=audio.channel(c)[i];const auto filtered=filters_[c].process(x);x=gain*(x+mix*(filtered-x));}}
 }
private:
 std::array<dsp::Biquad,2> filters_;dsp::SmoothedValue gain_,mix_;double rate_=48000,cutoff_=-1,q_=-1;
};
class Gallery final:public Editor {
public:
 explicit Gallery(Engine& engine):engine_(engine),gestures_(engine){}
 ~Gallery()override{cancelInput();}
 Point preferredSize()const noexcept override{return {1000,640};}
 bool preserveAspectRatio()const noexcept override{return false;}
 Point constrainSize(Point size)const noexcept override{return {std::clamp(size.x,420.,1600.),std::clamp(size.y,touch_?700.:600.,1600.)};}
 const char* title()const noexcept override{return "Cell / Control Development Gallery";}
 std::span<const ControlInfo> controls()const noexcept override{return controls_;}
 std::uint64_t focusedControl()const noexcept override{return focus_<controls_.size()?controls_[focus_].identity:0;}
 void options(bool touch,bool character,bool faders,bool light){touch_=touch;character_=character;faders_=faders;light_=light;}
 void paint(Canvas& canvas,Rect bounds)override{
  width_=bounds.width;height_=bounds.height;controls_.clear();Palette palette;
  if(light_){palette.background=Color(0xffedf1ef);palette.panel=Color(0xfff8faf8);palette.surface=Color(0xffdbe5e2);palette.edge=Color(0xff718681);palette.ink=Color(0xff142923);palette.muted=Color(0xff4a625a);palette.accent=Color(0xff007b68);}
  const auto metrics=touch_?Metrics::touch():Metrics{};canvas.fill(bounds,palette.background);
  centeredText(canvas,{bounds.x+20,bounds.y+12,bounds.width-40,32},"CELL / CONTROLS",18,palette.ink);
  const double buttonWidth=(bounds.width-52)/4,buttonHeight=touch_?44.:30.;
  const std::array<std::string,4> captions{character_?"CHARACTER":"ANALYTICAL",faders_?"FADERS":"KNOBS",touch_?"TOUCH":"POINTER",light_?"LIGHT":"DARK"};
  for(unsigned i=0;i<4;++i){const Rect area{bounds.x+20+i*(buttonWidth+4),bounds.y+52,buttonWidth,buttonHeight};button(canvas,area,captions[i],{true,false,false,focus_==i},palette,metrics);ControlInfo info;info.identity=i+1;info.role=ControlInfo::Role::button;info.bounds=area;info.label=info.accessibleName=captions[i];controls_.push_back(std::move(info));}
  auto grid=controlGrid({bounds.x+20,bounds.y+buttonHeight+72,bounds.width-40,0},4,4,metrics);
  for(unsigned i=0;i<4;++i){const auto& field=engine_.descriptor().parameters[i];const double value=engine_.parameters().get(field.id);const auto text=editing_&&focusedParameter()==field.id?entry_+"_":formatValue(field,value,formats[i]);const bool focused=focus_>=4&&(focus_-4)/2==i;
   const VisualState state{true,false,pressed(field.id),focused,false,!message_.empty()&&focused};
   const auto geometry=faders_?parameterFader(canvas,grid.cell(i),field.name,text,field.toNormalized(value),state,palette,metrics):parameterKnob(canvas,grid.cell(i),field.name,text,field.toNormalized(value),state,palette,metrics,character_?Presentation::character:Presentation::analytical);
   const auto infos=parameterInfo(field,value,field.name,formatValue(field,value,formats[i]),geometry,true,faders_?2:1);controls_.insert(controls_.end(),infos.begin(),infos.end());
  }
  const double top=grid.bounds.y+grid.height()+20;const Rect graph{bounds.x+20,top,bounds.width-40,std::max(60.,bounds.height-top+bounds.y-68)};
  canvas.rounded(graph,6,palette.panel);canvas.save();canvas.clip(graph);
  for(int db:{-48,-24,0,12}){const double y=graph.y+(12-db)/72.*graph.height;canvas.line({graph.x,y},{graph.x+graph.width,y},palette.edge,.5);}
  updateCurve();std::array<Point,256> line;for(unsigned i=0;i<line.size();++i)line[i]={graph.x+graph.width*i/(line.size()-1),graph.y+(12-curve_[i])/72.*graph.height};canvas.polyline(line,palette.accent,2);canvas.restore();
  centeredText(canvas,{bounds.x+20,bounds.y+bounds.height-42,bounds.width-40,28},message_.empty()?"TAB: FOCUS   ·   SHIFT: FINE   ·   ENTER: TYPE   ·   CTRL/CMD+0: RESET":message_,11,message_.empty()?palette.muted:palette.error);
 }
 bool pointer(const PointerEvent& event)override{
  if(auto* contact=findContact(event.identity)){
   if(event.action==PointerAction::up||event.action==PointerAction::cancel){if(gestures_.finish(event.identity))contact->used=false;notify();return true;}
   if(event.action==PointerAction::move){const auto* field=engine_.parameters().find(contact->id);const double movement=contact->fader?event.position.x-contact->last.x:contact->last.y-event.position.y;const auto value=field->fromNormalized(std::clamp(field->toNormalized(engine_.parameters().get(contact->id))+movement/(event.fine?1800.:180.),0.,1.));const std::array<std::pair<ParamId,double>,1> edits{{{contact->id,value}}};gestures_.set(event.identity,edits);contact->last=event.position;notify();return true;}
  }
  if(event.action==PointerAction::cancel){cancelInput();return true;}
  for(unsigned i=0;i<controls_.size();++i)if(controls_[i].bounds.contains(event.position)){
   if(event.action==PointerAction::wheel&&controls_[i].id){focus_=i;return adjust(event.wheel>0?1:-1,event.fine);}
   if(event.action!=PointerAction::down)return false;focus_=i;message_.clear();entry_.clear();editing_=false;
   if(i<4){activate(i);return true;}
   const auto id=controls_[i].id;if(event.reset)return set(id,engine_.parameters().find(id)->defaultValue);
   if(controls_[i].role==ControlInfo::Role::textbox){editing_=true;notify();return true;}
   auto* contact=freeContact();if(!contact||!gestures_.begin(event.identity,std::array<ParamId,1>{id}))return false;*contact={true,event.identity,id,event.position,faders_};notify();return true;
  }return false;
 }
 void cancelInput()override{gestures_.cancelAll();for(auto& contact:contacts_)contact.used=false;editing_=false;entry_.clear();message_.clear();notify();}
 void visibility(bool visible)override{if(!visible)cancelInput();Editor::visibility(visible);}
 bool keyboard(const KeyEvent& event)override{
  const int key=event.key;
  if(key==27){cancelInput();return true;}
  if(key==9||key==keys::reverseTab){cancelInput();if(!controls_.empty())focus_=(focus_+controls_.size()+((event.shift||key==keys::reverseTab)?-1:1))%controls_.size();notify();return true;}
  if(focus_>=controls_.size())return false;
  if(focus_<4){if(key==13||key==' '){activate(focus_);return true;}return false;}
  const auto id=focusedParameter();const auto* field=engine_.parameters().find(id);if(!field)return false;
  if((event.control||event.command)&&(key=='0'))return set(id,field->defaultValue);
  if((event.control||event.command)&&(key=='c'||key=='C'))return clipboard_.copy(engine_.parameters(),id);
  if((event.control||event.command)&&(key=='v'||key=='V')){if(const auto value=clipboard_.paste(*field))return set(id,*value);return false;}
  if(key==keys::up||key==keys::right||key==keys::down||key==keys::left)return adjust(key==keys::up||key==keys::right?1:-1,event.shift);
  if(key==keys::home||key==keys::end)return set(id,key==keys::home?field->minimum:field->maximum);
  if(key==13){if(!editing_){editing_=true;entry_.clear();notify();return true;}double value{};const auto index=unsigned((focus_-4)/2);if(parseValue(*field,entry_,formats[index],value)){if(set(id,value)){editing_=false;entry_.clear();}}else message_="ENTER A COMPLETE VALUE WITH THE CORRECT UNIT";notify();return true;}
  if(editing_&&key==8){if(!entry_.empty())entry_.pop_back();message_.clear();notify();return true;}
  if(editing_&&key>=32&&key<127&&!event.control&&!event.command){if(entry_.size()<64)entry_+=char(key);message_.clear();notify();return true;}
  return false;
 }
 bool accessibleAction(std::uint64_t identity,AccessibleAction action,double value)override{
  for(unsigned i=0;i<controls_.size();++i)if(controls_[i].identity==identity){focus_=i;
   if(action==AccessibleAction::focus){notify();return true;}if(action==AccessibleAction::activate){if(i<4)activate(i);else{editing_=true;entry_.clear();notify();}return true;}
   if(i<4)return false;if(action==AccessibleAction::setValue){const auto* field=engine_.parameters().find(controls_[i].id);return std::isfinite(value)&&value>=field->minimum&&value<=field->maximum&&set(field->id,value);}return adjust(action==AccessibleAction::increment?1:-1,false);
  }return false;
 }
private:
 ParamId focusedParameter()const{return focus_<controls_.size()?controls_[focus_].id:0;}
 void activate(unsigned index){cancelInput();if(index==0)character_=!character_;if(index==1)faders_=!faders_;if(index==2){touch_=!touch_;if(requestSize)requestSize(int(width_),int(std::max(height_,touch_?700.:600.)));}if(index==3)light_=!light_;notify();}
 bool set(ParamId id,double value){if(!gestures_.cancelAll()||!engine_.editGesture(id,value)){message_="CONTROL QUEUE BUSY / TRY AGAIN";notify();return false;}for(auto& contact:contacts_)contact.used=false;editing_=false;entry_.clear();message_.clear();notify();return true;}
 bool adjust(int direction,bool fine){const auto id=focusedParameter();const auto* field=engine_.parameters().find(id);if(!field)return false;return set(id,field->fromNormalized(std::clamp(field->toNormalized(engine_.parameters().get(id))+direction*(fine?.001:.01),0.,1.)));}
 void notify(){if(editsAvailable)editsAvailable();if(invalidate)invalidate();}
 void updateCurve(){
  std::array<double,4> key{};engine_.parameters().copyValues(key);if(key==curveKey_)return;curveKey_=key;
  const auto c=dsp::BiquadCoefficients::lowpass(48000,key[1],key[2]);const double gain=std::pow(10.,key[0]/20),mix=key[3]*.01;
  for(unsigned i=0;i<curve_.size();++i){const double frequency=20*std::pow(1000.,double(i)/(curve_.size()-1));const auto z=std::polar(1.,-2*std::numbers::pi*frequency/48000);const auto h=(c.b0+c.b1*z+c.b2*z*z)/(1.+c.a1*z+c.a2*z*z);curve_[i]=20*std::log10(std::max(1.e-12,std::abs((1.-mix)+mix*h)*gain));}
 }
 Engine& engine_;GestureBroker gestures_;ParameterClipboard clipboard_;std::vector<ControlInfo> controls_;
 struct Contact{bool used{};PointerIdentity pointer;ParamId id{};Point last;bool fader{};};
 Contact* findContact(PointerIdentity pointer){for(auto& contact:contacts_)if(contact.used&&contact.pointer==pointer)return &contact;return nullptr;}
 Contact* freeContact(){for(auto& contact:contacts_)if(!contact.used)return &contact;return nullptr;}
 bool pressed(ParamId id)const{for(const auto& contact:contacts_)if(contact.used&&contact.id==id)return true;return false;}
 std::array<Contact,GestureBroker::maxPointers> contacts_{};unsigned focus_=4;bool touch_{},character_{},faders_{},light_{},editing_{};
 double width_=1000,height_=640;std::string entry_,message_;std::array<double,4> curveKey_{-999,-999,-999,-999};std::array<double,256> curve_{};
};
}
int main(int argc,char** argv){try{
 Engine engine(descriptor,std::make_unique<GalleryProcessor>());engine.prepare(48000,256);
 Gallery editor(engine);editor.editsAvailable=[&]{ParameterEdit edit;while(engine.pollUiEdit(edit)){};};
 if(argc==3&&std::string_view(argv[1])=="--render"){
  const std::filesystem::path destination=argv[2];std::filesystem::create_directories(destination);
  for(bool portrait:{false,true})for(bool touch:{false,true})for(bool character:{false,true})for(bool faders:{false,true}){
   editor.options(touch,character,faders,false);Surface surface;surface.resize(portrait?620:1000,portrait?980:touch?720:640);surface.render([&](Canvas& canvas){editor.paint(canvas,{0,0,double(surface.width()),double(surface.height())});});
   const std::string name=std::string(portrait?"portrait":"landscape")+(touch?"-touch":"-pointer")+(character?"-character":"-analytical")+(faders?"-faders":"-knobs")+".png";
   if(!surface.writePng((destination/name).string()))throw std::runtime_error("Could not write gallery image");
  }return 0;
 }
 Window window(editor);if(!window.open(0,1000,640))throw std::runtime_error("Native display is unavailable");window.show(true);
 while(window.isOpen()){window.poll();std::this_thread::sleep_for(std::chrono::milliseconds(16));}
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
