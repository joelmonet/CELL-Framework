// Source-only additions. Native compilation and execution remain on hold.
#include <cell/ui_interaction.hpp>
#include "allocation_watch.hpp"
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Pass final:Processor{void prepare(const ProcessSpec&)override{}void reset()noexcept override{}void process(AudioBlock<float>,const ParameterBank&,const Transport&,std::span<const MidiEvent>)noexcept override{}};
std::vector<Parameter> fields(){std::vector<Parameter> result;for(unsigned i=0;i<64;++i){const auto key="p"+std::to_string(i);result.push_back({100+i,key,key,"DB",-100,100,0,0,1});}return result;}
Engine make(){const auto parameters=fields();return Engine({"cell.interaction.test","Interaction","Cell","1",2,parameters},std::make_unique<Pass>());}
void drain(Engine& e){ParameterEdit value;while(e.pollEdit(value)){};}
void admission(){
 auto e=make();const std::array<ParamId,2> duplicate{100,100};check(!e.beginEdits(duplicate)&&e.activeGestures()==0,"duplicate parameter begin was admitted");ParameterEdit event;check(!e.pollEdit(event),"rejected begin published an event");
 const std::array<ParamId,3> ids{100,101,102};check(e.beginEdits(ids),"batch begin failed");const std::array<ParamId,2> collision{101,103};check(!e.beginEdits(collision)&&!e.gestureActive(103)&&e.activeGestures()==3,"overlapping begin partially admitted");
 const std::array<std::pair<ParamId,double>,2> invalid{{{100,40},{101,NAN}}};check(!e.editValues(invalid)&&e.parameters().get(100)==0,"invalid batch partially changed parameter bank");
 const std::array<std::pair<ParamId,double>,2> valid{{{100,20},{101,-30}}};check(e.editValues(valid)&&e.parameters().get(100)==20&&e.parameters().get(101)==-30,"independent values failed");
 const std::array<ParamId,2> wrongEnd{100,163};check(!e.endEdits(wrongEnd)&&e.activeGestures()==3,"invalid close removed another active parameter");
 while(e.edit({EditType::value,100,21})){};
 check(!e.edit({EditType::begin,103,0})&&e.activeGestures()==3,"begin consumed reserved close capacity");
 check(e.endEdits(ids)&&e.activeGestures()==0,"full queue lost reserved closes");
 std::array<bool,64> open{};unsigned ends=0;while(e.pollEdit(event)){const auto i=event.id-100;check(i<open.size(),"wrong parameter in edit queue");if(event.type==EditType::begin){check(!open[i],"duplicate host gesture");open[i]=true;}if(event.type==EditType::end){check(open[i],"close without begin");open[i]=false;++ends;}}
 check(ends==3&&std::none_of(open.begin(),open.end(),[](bool x){return x;}),"unbalanced host gestures");
 std::array<ParamId,Engine::maxConcurrentGestures> all;for(unsigned i=0;i<all.size();++i)all[i]=100+i;check(e.beginEdits(all),"maximum simultaneous gesture batch rejected");check(!e.beginEdits(std::array<ParamId,1>{163}),"gesture capacity was exceeded");while(e.edit({EditType::value,100,1})){};check(e.endAllEdits()&&e.activeGestures()==0,"endAll aliased its active-ID list or lost reservations");drain(e);
}
void momentary(){
 auto e=make();check(e.beginEdits(std::array<ParamId,1>{100}),"normal gesture admission");
 check(e.beginMomentary(101,77,-25)&&e.beginMomentary(102,1,0),"momentary controls not admitted alongside a normal gesture");
 check(e.parameters().get(101)==77&&!e.beginMomentary(101,99,0)&&!e.beginMomentary(103,NAN,0)&&!e.beginMomentary(103,1,NAN)&&e.parameters().get(103)==0,"invalid momentary admission changed the bank or ownership");
 while(e.edit({EditType::value,100,4})){};
 check(!e.beginMomentary(103,1,0)&&e.parameters().get(103)==0,"momentary press consumed release reservations");
 check(e.endEdits(std::array<ParamId,1>{101})&&e.parameters().get(101)==-25,"full queue lost momentary release value");
 check(e.endAllEdits()&&e.activeGestures()==0&&e.parameters().get(102)==0,"mixed endAll lost a release or aliased its ID span");
 ParameterEdit event;std::array<bool,3> open{};std::array<double,3> last{};unsigned ends=0;
 while(e.pollEdit(event)){const auto i=event.id-100;check(i<3,"unexpected momentary event identity");if(event.type==EditType::begin){check(!open[i],"duplicate begin");open[i]=true;}else if(event.type==EditType::value){check(open[i],"value outside momentary gesture");last[i]=event.value;}else{check(open[i],"end without begin");if(i==1)check(last[i]==-25,"off value was not delivered before end");if(i==2)check(last[i]==0,"second off value missing");open[i]=false;++ends;}}
 check(ends==3,"missing normal/momentary closes");
 cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;const bool press=e.beginMomentary(101,1,0),release=e.endAllEdits();cell_test::audio=false;check(press&&release&&!cell_test::allocations&&!cell_test::deletions,"momentary fixed-storage path allocated");drain(e);
}
void contacts(){
 auto e=make();ui::GestureBroker broker(e);const ui::PointerIdentity a{1,ui::PointerKind::touch},b{2,ui::PointerKind::touch},pen{1,ui::PointerKind::pen};
 check(broker.begin(a,std::array<ParamId,2>{100,101})&&broker.begin(b,std::array<ParamId,1>{102})&&broker.begin(pen,std::array<ParamId,1>{103}),"independent contacts or device kind identity failed");
 check(!broker.begin(a,std::array<ParamId,1>{104})&&!broker.begin({3,ui::PointerKind::touch},std::array<ParamId,1>{100})&&e.activeGestures()==4,"duplicate pointer or parameter ownership stolen");
 check(!broker.set(a,std::array<std::pair<ParamId,double>,1>{{{102,50}}})&&e.parameters().get(102)==0,"contact wrote another contact's parameter");
 check(broker.set(a,std::array<std::pair<ParamId,double>,2>{{{100,12},{101,24}}})&&broker.set(b,std::array<std::pair<ParamId,double>,1>{{{102,-3}}}),"multi-axis contact values failed");
 check(broker.finish(a)&&e.activeGestures()==2&&broker.active(b)&&broker.active(pen)&&e.parameters().get(100)==12,"one cancellation ended or rolled back another contact");
 const auto saved=e.saveState();const auto generation=e.stateGeneration();check(e.loadState(saved)&&e.stateGeneration()!=generation,"restore generation did not advance");check(!broker.set(b,std::array<std::pair<ParamId,double>,1>{{{102,50}}})&&!broker.active(b)&&e.parameters().get(102)==-3,"stale contact changed restored state");check(broker.cancelAll()&&e.activeGestures()==0,"cancelAll left active host gestures");drain(e);
 cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;const bool started=broker.begin(a,std::array<ParamId,2>{100,101});const bool changed=broker.set(a,std::array<std::pair<ParamId,double>,2>{{{100,4},{101,5}}});const bool ended=broker.cancelAll();cell_test::audio=false;check(started&&changed&&ended&&!cell_test::allocations&&!cell_test::deletions,"fixed broker paths allocated or failed");drain(e);
 for(unsigned i=0;i<ui::GestureBroker::maxPointers;++i)check(broker.begin({i,ui::PointerKind::touch},std::array<ParamId,1>{100+i}),"contact capacity smaller than declared");check(!broker.begin({99,ui::PointerKind::touch},std::array<ParamId,1>{163}),"contact capacity exceeded");check(broker.cancelAll()&&e.activeGestures()==0,"maximum contact cancellation failed");
}
void valueScrubbing(){
 ui::ValueDrag gain(0,100,-72,72,.1);double y=100;for(int n=0;n<100;++n){y-=.1;gain.move(y,true);}check(std::abs(gain.value()-.1)<1e-9,"Fine value scrub discarded sub-step movements");const auto before=gain.value();check(gain.move(y)==before,"Changing fine mode without movement jumped the value");
 ui::ValueDrag frequency(1000,10,5,40000,.015,ui::ValueDrag::Scale::logarithmic);check(std::abs(frequency.move(-10)-1000*std::exp(.3))<1e-8,"Frequency scrub was not logarithmic");
 ui::ValueDrag slope(1,10,0,3,.05,ui::ValueDrag::Scale::stepped);check(slope.move(-10)==2&&slope.move(10)==1,"Slope scrub lost fractional accumulation or reverse movement");
}
void rotaryMotion(){
 ui::RotaryMotion rotary;check(rotary.delta(0,0)==0,"Stationary knob selected an axis");
 check(rotary.delta(1,-1)==-1&&rotary.delta(1,-2)==-2,"Diagonal rotary motion cancelled or accelerated");
 check(rotary.delta(100,0)==0,"Secondary-axis drift altered a vertical gesture");check(rotary.delta(-1,1)==1,"Rotary reversal lost its sign");
 rotary.reset();check(rotary.delta(.1,.01)==.1&&rotary.delta(.1,-.1)==.1,"Horizontal fine motion did not retain its fractional axis");check(rotary.delta(NAN,0)==0,"Non-finite pointer movement was accepted");
}
void focusAndClipboard(){
 const std::array<ui::FocusTarget,4> targets{{{1,true},{2,false},{3,true},{4,false}}};check(ui::moveFocus(targets,1)==2&&ui::moveFocus(targets,1,true)==2,"focus did not skip disabled controls or wrap backwards");check(ui::moveFocus(targets,0)==0&&ui::moveFocus(targets,0,true)==2,"first/reverse focus entry wrong");check(!ui::moveFocus({},0),"empty focus list admitted");
 auto e=make();ui::ParameterClipboard clipboard;check(!clipboard.canPaste(*e.parameters().find(100)),"empty clipboard paste enabled");e.parameters().set(100,7.5);check(clipboard.copy(e.parameters(),100)&&clipboard.paste(*e.parameters().find(101))==7.5,"compatible value copy lost physical value");auto incompatible=*e.parameters().find(101);incompatible.unit="MS";check(!clipboard.canPaste(incompatible),"incompatible unit paste enabled");incompatible.unit="DB";incompatible.maximum=5;check(!clipboard.canPaste(incompatible),"out-of-range paste silently clipped");
}
void concurrentDelivery(){
 auto e=make();std::atomic<bool> done{},failed{};std::thread consumer([&]{std::array<bool,64> open{};ParameterEdit edit;bool finished=false;for(;;){if(e.pollUiEdit(edit)){const auto index=edit.id-100;if(index>=open.size()){failed=true;continue;}if(edit.type==EditType::begin){if(open[index])failed=true;open[index]=true;}else if(edit.type==EditType::end){if(!open[index])failed=true;open[index]=false;}else if(!open[index])failed=true;}else if(finished)break;else if(done.load(std::memory_order_acquire))finished=true;else std::this_thread::yield();}for(bool value:open)if(value)failed=true;});
 const std::array<ParamId,4> ids{100,101,102,103};for(unsigned n=0;n<1000;++n){while(!e.beginEdits(ids))std::this_thread::yield();const std::array<std::pair<ParamId,double>,4> values{{{100,1},{101,2},{102,3},{103,4}}};while(!e.editValues(values))std::this_thread::yield();if(!e.endEdits(ids)){failed=true;break;}}
 done.store(true,std::memory_order_release);consumer.join();check(!failed&&!e.activeGestures(),"concurrent consumer observed unbalanced or reordered gestures");
}
}
int main(){try{admission();momentary();contacts();valueScrubbing();rotaryMotion();focusAndClipboard();concurrentDelivery();std::cout<<"PASS concurrent parameter ownership, reserved closes, cancellation, focus and clipboard contracts\n";}catch(const std::exception& error){cell_test::audio=false;std::cerr<<error.what()<<'\n';return 1;}}
