#include "gain.hpp"
#include <cell/ui_components.hpp>
#include <cell/ui_interaction.hpp>

namespace cell::example {
class GainEditor final:public ui::Editor {
public:
 explicit GainEditor(Engine& engine):engine_(engine),gestures_(engine){}
 ~GainEditor()override{cancelInput();}
 ui::Point preferredSize()const noexcept override{return {480,320};}
 bool preserveAspectRatio()const noexcept override{return false;}
 ui::Point constrainSize(ui::Point value)const noexcept override{return {std::clamp(value.x,280.,1000.),std::clamp(value.y,260.,800.)};}
 void paint(ui::Canvas& canvas,ui::Rect bounds)override{
  using namespace ui::components;Palette palette;canvas.fill(bounds,palette.background);canvas.rounded(bounds.inset(12),8,palette.panel);
  centeredText(canvas,{bounds.x+24,bounds.y+22,bounds.width-48,30},"GAIN",16,palette.ink);
  const auto& parameter=field();const double value=engine_.parameters().get(gainId);
  const ui::Rect tile{bounds.x+(bounds.width-160)/2,bounds.y+(bounds.height-132)/2,160,132};
  const auto geometry=parameterKnob(canvas,tile,"LEVEL",entry_.empty()?ui::formatValue(parameter,value,format_):entry_+"_",parameter.toNormalized(value),{true,hovered_,gestures_.size()>0,focused_,false,invalid_});
  controls_=parameterInfo(parameter,value,"Gain",ui::formatValue(parameter,value,format_),geometry);
  ui::components::centeredText(canvas,{bounds.x+16,bounds.y+bounds.height-44,bounds.width-32,24},invalid_?"ENTER −60 TO +12 dB":"SHIFT: FINE   ·   ENTER: TYPE   ·   CTRL+0: RESET",11,invalid_?palette.error:palette.muted);
 }
 std::span<const ui::ControlInfo> controls()const noexcept override{return controls_;}
 std::uint64_t focusedControl()const noexcept override{return focused_?controls_[focus_].identity:0;}
 bool pointer(const ui::PointerEvent& event)override{
  if(gestures_.active(event.identity)){
   if(event.action==ui::PointerAction::up||event.action==ui::PointerAction::cancel){gestures_.finish(event.identity);notify();return true;}
   if(event.action==ui::PointerAction::move){const double n=field().toNormalized(engine_.parameters().get(gainId))+(last_.y-event.position.y)/(event.fine?1800:180);const std::array<std::pair<ParamId,double>,1> values{{{gainId,field().fromNormalized(std::clamp(n,0.,1.))}}};gestures_.set(event.identity,values);last_=event.position;notify();return true;}
  }
  const bool slider=controls_[0].bounds.contains(event.position),text=controls_[1].bounds.contains(event.position);
  if(event.action==ui::PointerAction::move){hovered_=slider||text;if(invalidate)invalidate();return hovered_;}
  if(event.action==ui::PointerAction::context&&(slider||text)&&hostParameterMenuAvailable&&hostParameterMenuAvailable(gainId)&&showHostParameterMenu)return showHostParameterMenu(gainId,event.position);
  if(event.action==ui::PointerAction::wheel&&(slider||text)){set(engine_.parameters().get(gainId)+event.wheel*(event.fine?.1:1));return true;}
  if(event.action!=ui::PointerAction::down||(!slider&&!text))return false;
  focused_=true;focus_=text?1:0;invalid_=false;entry_.clear();
  if(event.reset){set(field().defaultValue);return true;}
  if(text){if(invalidate)invalidate();return true;}
  if(!gestures_.begin(event.identity,std::array<ParamId,1>{gainId}))return false;
  last_=event.position;notify();return true;
 }
 void cancelInput()override{gestures_.cancelAll();entry_.clear();invalid_=false;notify();}
 void visibility(bool visible)override{if(!visible)cancelInput();Editor::visibility(visible);}
 bool keyboard(const ui::KeyEvent& event)override{
  const int key=event.key;
  if(key==27){cancelInput();return true;}
  if(key==9||key==ui::keys::reverseTab){focused_=true;focus_=1-focus_;entry_.clear();invalid_=false;if(invalidate)invalidate();return true;}
  if(!focused_)return false;
  if((event.control||event.command)&&key=='0'){set(field().defaultValue);return true;}
  if((event.control||event.command)&&(key=='c'||key=='C'))return clipboard_.copy(engine_.parameters(),gainId);
  if((event.control||event.command)&&(key=='v'||key=='V')){if(auto value=clipboard_.paste(field()))return set(*value);return false;}
  if(key==ui::keys::home||key==ui::keys::end)return set(key==ui::keys::home?field().minimum:field().maximum);
  if(key==ui::keys::up||key==ui::keys::right||key==ui::keys::down||key==ui::keys::left)return set(engine_.parameters().get(gainId)+(key==ui::keys::up||key==ui::keys::right?1:-1)*(event.shift?.1:1));
  if(key==13){if(entry_.empty()){focus_=1;return true;}double value{};if(ui::parseValue(field(),entry_,format_,value)&&set(value)){entry_.clear();invalid_=false;}else invalid_=true;if(invalidate)invalidate();return true;}
  if(focus_==1&&key==8){if(!entry_.empty())entry_.pop_back();invalid_=false;if(invalidate)invalidate();return true;}
  if(focus_==1&&key>=32&&key<127&&!event.control&&!event.command){if(entry_.size()<64)entry_+=char(key);invalid_=false;if(invalidate)invalidate();return true;}
  return false;
 }
 bool accessibleAction(std::uint64_t identity,ui::AccessibleAction action,double value)override{
  for(unsigned i=0;i<controls_.size();++i)if(controls_[i].identity==identity){focused_=true;focus_=i;if(action==ui::AccessibleAction::focus){if(invalidate)invalidate();return true;}if(action==ui::AccessibleAction::activate){focus_=1;return true;}if(action==ui::AccessibleAction::setValue){if(!std::isfinite(value)||value<field().minimum||value>field().maximum)return false;return set(value);}return set(engine_.parameters().get(gainId)+(action==ui::AccessibleAction::increment?1:-1));}return false;
 }
private:
 const Parameter& field()const{return *engine_.parameters().find(gainId);}
 bool set(double value){if(!gestures_.cancelAll()||!engine_.editGesture(gainId,value))return false;entry_.clear();invalid_=false;notify();return true;}
 void notify(){if(editsAvailable)editsAvailable();if(invalidate)invalidate();}
 Engine& engine_;ui::GestureBroker gestures_;ui::ParameterClipboard clipboard_;
 std::array<ui::ControlInfo,2> controls_{};ui::Point last_;
 ui::ValueFormat format_{ui::ValueUnit::decibels,1};
 std::string entry_;unsigned focus_{};bool focused_{},hovered_{},invalid_{};
};
}
namespace cell::plugin {
std::unique_ptr<ui::Editor> createEditor(Engine& engine){return std::make_unique<example::GainEditor>(engine);}
}
