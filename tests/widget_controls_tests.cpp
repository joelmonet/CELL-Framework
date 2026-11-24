// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/controls.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {void check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
struct Root:cell::ui::widgets::Component{void paint(cell::ui::widgets::Graphics&g)override{g.fillAll(cell::ui::widgets::Colour{0xff14191a});}};}
int main(){using namespace cell::ui::widgets;using cell::ui::PointerAction;try{
    Root root;root.setSize(320,220);
    TextButton button("Keep");button.setBounds(20,20,100,28);button.setClickingTogglesState(true);int clicks=0;button.onClick=[&]{++clicks;};root.addAndMakeVisible(button);
    Slider slider;slider.setBounds(20,75,200,30);slider.setRange(0,100,0);slider.setSliderSnapsToMousePosition(false);slider.setValue(50,dontSendNotification);root.addAndMakeVisible(slider);int starts=0,ends=0;slider.onDragStart=[&]{++starts;};slider.onDragEnd=[&]{++ends;};
    TextEditor field;field.setBounds(20,125,240,28);field.setText("Snare",false);root.addAndMakeVisible(field);
    WidgetHost host(root);cell::ui::Surface pixels;pixels.resize(320,220);pixels.render([&](cell::ui::Canvas&c){host.paint(c,{0,0,320,220});});
    host.pointer({PointerAction::down,{40,32}});host.pointer({PointerAction::up,{40,32}});check(clicks==1&&button.getToggleState(),"Native button click/toggle did not reach the controller");
    host.pointer({PointerAction::down,{40,87}});host.pointer({PointerAction::move,{90,87}});host.pointer({PointerAction::up,{90,87}});check(slider.getValue()>50&&starts==1&&ends==1,"Native slider did not retain one drag lifetime");
    host.pointer({PointerAction::down,{80,87}});slider.setVisible(false);check(ends==2&&!slider.isMouseButtonDown(),"Hiding a captured slider stranded its edit gesture");
    field.grabKeyboardFocus();host.keyboard({'a',false,true});Clipboard::setText("Snaré");host.keyboard({'v',false,true});host.keyboard({'!'});check(field.getText()==Text{"Snaré!"},"Native text selection/paste damaged UTF-8");
    host.keyboard({'z',false,true});check(field.getText()==Text{"Snaré"},"Native text undo changed the wrong input action");host.keyboard({'Z',true,true});check(field.getText()==Text{"Snaré!"},"Native text redo did not restore the input action");
    host.keyboard({'s'});check(KeyPress::isKeyCurrentlyDown('S'),"Momentary key state was unavailable to the editor");host.keyReleased({'s'});check(!KeyPress::isKeyCurrentlyDown('s'),"Released split key stayed held");
    Label editable;editable.setBounds(20,170,160,28);editable.setEditable(true,true);editable.setText("Before",dontSendNotification);root.addAndMakeVisible(editable);editable.showEditor();editable.getCurrentTextEditor()->setText("After",false);editable.getCurrentTextEditor()->keyPressed(KeyPress{KeyPress::returnKey});check(!editable.isBeingEdited()&&editable.getText()==Text{"After"},"Native label did not commit on Enter");Timer::dispatchPending();
    auto removed=std::make_unique<TextButton>("Remove self");removed->setBounds(180,20,120,28);removed->onClick=[&]{removed.reset();};root.addAndMakeVisible(*removed);host.pointer({PointerAction::down,{200,32}});host.pointer({PointerAction::up,{200,32}});check(!removed,"A button could not safely remove its own panel");
    pixels.render([&](cell::ui::Canvas&c){host.paint(c,{0,0,320,220});});check(!host.tick(),"Unchanged native widgets kept invalidating the frame");
    std::cout<<"PASS Cell native widgets: real control callbacks, toggle, drag/cancel lifetime, UTF-8 text, local undo/redo, momentary keys, deferred field close and self-removal\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
