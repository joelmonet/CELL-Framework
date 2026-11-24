// SPDX-License-Identifier: Apache-2.0
#include <cell/popup_menu.hpp>
#include <iostream>
#include <stdexcept>
using namespace cell::ui;
int main(){try{
 // A worker can finish between open() and the first paint. Its lease must
 // already be valid, without depending on old visible row metadata.
 {PopupMenu pending;PopupMenu::Item row;row.text=row.label="READING";pending.open({row},{10,10});const auto first=pending.generation();
  if(!first||!pending.isCurrent(first)||!pending.controls().empty()||pending.focusedControl())throw std::runtime_error("Menu lease was not valid before its first paint");
  Surface frame;frame.resize(320,240);frame.render([&](Canvas& canvas){pending.paint(canvas,{0,0,320,240});});if(pending.controls().empty())throw std::runtime_error("Pending menu did not paint");
  pending.key(keys::down);if(!pending.isCurrent(first))throw std::runtime_error("Menu navigation invalidated its own async request");
  pending.open({row},{20,20});const auto second=pending.generation();if(first==second||pending.isCurrent(first)||!pending.isCurrent(second)||!pending.controls().empty()||pending.focusedControl())throw std::runtime_error("Replacing a menu retained stale rows or accepted the old worker lease");
  pending.close();if(pending.isCurrent(second))throw std::runtime_error("Closing a menu retained an async lease");pending.open({row},{30,30});if(pending.isCurrent(first)||pending.isCurrent(second))throw std::runtime_error("Reopened menu accepted a closed lifetime");}
 Surface surface;surface.resize(1040,760);PopupMenu menu;std::vector<PopupMenu::Item> items;
 for(int n=0;n<9;++n){PopupMenu::Item item;item.text=item.label="Slope "+std::to_string(n);item.command=n+1;items.push_back(item);}
 menu.open(items,{100,600});surface.render([&](Canvas& c){menu.paint(c,{0,0,1040,760});});
 if(menu.controls().size()!=9||menu.controls().front().bounds.y>=600)throw std::runtime_error("Popup clipped below its anchor despite enough room above");
 menu.open(items,{100,100});surface.render([&](Canvas& c){menu.paint(c,{0,0,1040,760});});
 if(menu.controls().size()!=9||menu.controls().front().bounds.y<100)throw std::runtime_error("Popup moved above an anchor that has room below");
 for(int n=9;n<36;++n){PopupMenu::Item item;item.text=item.label="Long item "+std::to_string(n);item.command=n+1;items.push_back(item);}
 for(Point at:{Point{250,230},Point{430,360}}){menu.open(items,at);surface.render([&](Canvas& c){menu.paint(c,{0,0,900,700});});const auto first=menu.controls().front().bounds;if(std::abs(first.x-at.x-6)>.01||std::abs(first.y-at.y-6)>.01)throw std::runtime_error("Scrolling menu lost click anchor");menu.key(keys::end);surface.render([&](Canvas& c){menu.paint(c,{0,0,900,700});});if(menu.key(13).command!=36)throw std::runtime_error("Scrolling menu lost last command");}

 const auto render=[&]{surface.render([&](Canvas& c){menu.paint(c,{0,0,1040,760});});};
 PopupMenu::Item family;family.text=family.label="BELL";family.selectFirstChild=true;
 PopupMenu::Item first;first.text=first.label="GENTLE";first.command=101;
 PopupMenu::Item second;second.text=second.label="STANDARD";second.command=102;family.children={first,second};
 const auto openFamily=[&]{menu.open({family},{100,100});render();};
 const auto point=[&]{const auto b=menu.controls().front().bounds;return Point{b.x+b.width*.5,b.y+b.height*.5};};
 const auto click=[&](Point p){menu.pointer({PointerAction::down,p});return menu.pointer({PointerAction::up,p});};
 openFamily();auto result=click(point());if(result.command!=101||menu.isOpen())throw std::runtime_error("Category click did not choose its first variant");
 openFamily();menu.pointer({PointerAction::move,point()});if(!menu.tick(PopupMenu::Clock::now()+std::chrono::milliseconds(200)))throw std::runtime_error("Category hover did not open variants");render();if(menu.controls().size()!=3||!menu.isOpen())throw std::runtime_error("Hover chose a variant instead of browsing");
 const auto child=menu.controls().back().bounds;result=click({child.x+child.width*.5,child.y+child.height*.5});if(result.command!=102)throw std::runtime_error("Explicit submenu variant was replaced by first variant");
 openFamily();const auto box=menu.controls().front().bounds;result=click({box.x+box.width-8,box.y+box.height*.5});render();if(result.command||!menu.isOpen()||menu.controls().size()!=3)throw std::runtime_error("Category arrow must browse without selecting");
 openFamily();if(menu.key(13).command!=101)throw std::runtime_error("Category Enter did not choose first variant");
 openFamily();menu.key(keys::right);menu.key(keys::down);if(menu.key(13).command!=102)throw std::runtime_error("Keyboard submenu navigation lost explicit variant");
 openFamily();if(menu.accessible(menu.controls().front().identity,AccessibleAction::activate).command!=101)throw std::runtime_error("Accessible category activation did not choose first variant");
 family.children.front().enabled=false;openFamily();if(click(point()).command!=102)throw std::runtime_error("Category chose a disabled variant");
 family.selectFirstChild=false;family.activateParent=true;family.command=900;openFamily();if(click(point()).command!=900)throw std::runtime_error("Existing parent activation changed");
 std::cout<<"PASS category first-variant click/Enter/accessibility, hover, arrow, explicit variant and disabled entries\n";
 std::cout<<"PASS fitting popup uses room above/below; long scrolling menu retains click anchor\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
