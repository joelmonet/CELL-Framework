// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/services.hpp>
#include <iostream>
using namespace cell::ui;namespace w=cell::ui::widgets;
namespace {
void need(bool v,const char*m){if(!v)throw std::runtime_error(m);}
struct Probe:w::Component {int clicks{},moves{};w::ModifierKeys mods;void mouseDown(const w::MouseEvent&e)override{mods=e.mods;++clicks;}void mouseDrag(const w::MouseEvent&e)override{mods=e.mods;++moves;}};
struct Rows:w::TableListBoxModel {
 int painted{},made{},refreshed{},selected=-1,deleted=-1;
 int getNumRows()override{return 30000;}
 void paintRowBackground(w::Graphics&,int,int,int,bool)override{++painted;}
 void paintCell(w::Graphics&,int,int,int,int,bool)override{}
 w::Component*refreshComponentForCell(int row,int,bool selected,w::Component*old)override{++refreshed;auto*label=dynamic_cast<w::Label*>(old);if(!label){label=new w::Label;++made;}label->setText(w::Text(row)+(selected?" selected":""),w::dontSendNotification);return label;}
 void selectedRowsChanged(int row)override{selected=row;}void deleteKeyPressed(int row)override{deleted=row;}
};
struct Painter:w::Component {std::function<void()>onPaint;void paint(w::Graphics&)override{if(onPaint)onPaint();}};
}
int main(){try{
 w::Component root;root.setSize(800,600);w::WidgetHost host(root);Probe probe;probe.setBounds(10,10,200,160);root.addAndMakeVisible(probe);
 PointerEvent down{PointerAction::down,{30,30}};down.control=true;down.alt=true;down.button=PointerButton::middle;need(host.pointer(down),"Native down not routed");need(probe.mods.isCtrlDown()&&probe.mods.isAltDown()&&probe.mods.isMiddleButtonDown(),"Pointer modifiers/middle button lost");PointerEvent drag{PointerAction::move,{1000,1000}};drag.control=true;host.pointer(drag);need(probe.moves==1&&probe.mods.isCtrlDown()&&!probe.mods.isAltDown(),"Drag capture or current modifiers lost outside viewport");host.pointer({PointerAction::up,{1000,1000}});need(!probe.isMouseButtonDown(),"Mouse-up outside viewport stranded capture");
 host.keyboard({17,false,true});host.keyReleased({17});need(!w::ModifierKeys::getCurrentModifiers().isCtrlDown(),"Control stuck after key release");
 w::TextButton first("First"),second("Second");first.setBounds(240,10,100,30);second.setBounds(350,10,100,30);root.addAndMakeVisible(first);root.addAndMakeVisible(second);first.grabKeyboardFocus();host.keyboard({9});need(second.hasKeyboardFocus(false),"Tab failed to advance keyboard focus");host.keyboard({9,true});need(first.hasKeyboardFocus(false),"Shift Tab failed to restore focus");
 Rows rows;w::TableListBox table;root.addAndMakeVisible(table);table.setBounds(0,200,800,350);table.getHeader().addColumn("Region",1,700);table.setModel(&rows);need(rows.made>0&&rows.made<20,"Large table allocated invisible row components");
 Surface surface;surface.resize(800,600);surface.render([&](Canvas&c){host.paint(c,{0,0,800,600});});need(rows.painted>0&&rows.painted<20,"Large table painted invisible rows");table.selectRow(24000);need(rows.selected==24000&&rows.made<40,"Table selection could not navigate distant rows efficiently");table.keyPressed(w::KeyPress(w::KeyPress::deleteKey));need(rows.deleted==24000,"Region delete keyboard action lost");need(table.getViewport()->getViewPositionY()>700000,"Table failed to navigate to distant selected row");
 auto owner=std::make_unique<w::TextButton>("Owner");owner->setBounds(600,10,100,30);root.addAndMakeVisible(*owner);int menuAction=0,callback=0;w::PopupMenu menu;menu.addItem("Unsafe if owner gone",[&]{++menuAction;});menu.showMenuAsync(w::PopupMenu::Options().withTargetComponent(owner.get()).withDeletionCheck(*owner),[&](int){++callback;});owner.reset();host.pointer({PointerAction::down,{620,52}});host.pointer({PointerAction::up,{620,52}});need(menuAction==0&&callback==0,"Closed menu owner still received an action");
 w::Component canvas;canvas.setSize(100,100);canvas.setVisible(true);auto dying=std::make_unique<Painter>();auto sibling=std::make_unique<Painter>();dying->setBounds(0,0,100,100);sibling->setBounds(0,0,100,100);int siblingPaints=0;sibling->onPaint=[&]{++siblingPaints;};dying->onPaint=[&]{sibling.reset();};canvas.addAndMakeVisible(*dying);canvas.addAndMakeVisible(*sibling);auto image=canvas.createComponentSnapshot(canvas.getLocalBounds());need(image.isValid()&&siblingPaints==0,"Paint used a destroyed sibling");
 w::Component layered;layered.setSize(8,8);layered.setVisible(true);layered.setAlpha(.5f);struct Fill:w::Component{void paint(w::Graphics&g)override{g.fillAll(w::Colour{0xffffffff});}}fill;fill.setBounds(0,0,8,8);fill.setAlpha(.5f);layered.addAndMakeVisible(fill);auto alpha=layered.createComponentSnapshot(layered.getLocalBounds(),true,1);w::Image visible(w::Image::ARGB,8,8,true);{w::Graphics g(visible);layered.paintEntireComponent(g);}const auto pixel=visible.getPixelAt(4,4).getARGB();need((pixel>>24)>=63&&(pixel>>24)<=65,"Nested component opacity did not multiply");
 std::cout<<"PASS native navigation: pointer modifiers, outside capture, key release, Tab focus, 30000 virtual rows, distant selection/delete, closed-owner menu, sibling removal and nested opacity\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
