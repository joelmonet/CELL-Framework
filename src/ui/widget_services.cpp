// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/services.hpp>
#include <chrono>
#include <deque>
#include <mutex>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#undef KeyPress
#endif
namespace cell::ui::widgets {
ListBox::ListBox(){setColour(backgroundColourId,Colour{0xff141719});setColour(outlineColourId,Colour{0xff343b3e});setWantsKeyboardFocus(true);table_.setModel(this);table_.setHeaderHeight(0);table_.getHeader().addColumn("",1,200,1);addAndMakeVisible(table_);}
void ListBox::setModel(ListBoxModel*m){model_=m;table_.updateContent();}
void ListBox::paint(Graphics&g){g.fillAll(findColour(backgroundColourId));if(outline_){g.setColour(findColour(outlineColourId));g.drawRect(getLocalBounds(),float(outline_));}}
void ListBox::resized(){table_.setBounds(getLocalBounds().reduced(outline_));table_.getHeader().setColumnWidth(1,std::max(1,table_.getWidth()));table_.updateContent();}
bool ListBox::keyPressed(const KeyPress&k){if(!model_)return false;const auto code=k.getKeyCode();if(code==KeyPress::returnKey){model_->returnKeyPressed(getSelectedRow());return true;}if(code==KeyPress::upKey||code==KeyPress::downKey||code==KeyPress::homeKey||code==KeyPress::endKey){const auto count=model_->getNumRows();if(!count)return true;const int row=code==KeyPress::homeKey?0:code==KeyPress::endKey?count-1:std::clamp(getSelectedRow()+(code==KeyPress::upKey?-1:1),0,count-1);selectRow(row);return true;}return false;}
Displays::Displays(){refresh();}
void Displays::refresh(){displays_.clear();
#if defined(_WIN32)
 EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM value)->BOOL{auto*self=reinterpret_cast<Displays*>(value);MONITORINFO info{sizeof(info)};if(GetMonitorInfo(monitor,&info)){const auto rect=[](const RECT&r){return Rectangle<int>{int(r.left),int(r.top),int(r.right-r.left),int(r.bottom-r.top)};};DisplayInfo d{rect(info.rcMonitor),rect(info.rcWork),1};if(info.dwFlags&MONITORINFOF_PRIMARY)self->displays_.insert(self->displays_.begin(),d);else self->displays_.push_back(d);}return TRUE;},reinterpret_cast<LPARAM>(this));
#elif defined(__linux__)
 if(auto*d=XOpenDisplay(nullptr)){const int screen=DefaultScreen(d);DisplayInfo info{{0,0,DisplayWidth(d,screen),DisplayHeight(d,screen)},{0,0,DisplayWidth(d,screen),DisplayHeight(d,screen)},1};
  Atom actual{};int format{};unsigned long count{},after{};unsigned char*data{};const auto property=XInternAtom(d,"_NET_WORKAREA",True);
  if(property&&XGetWindowProperty(d,RootWindow(d,screen),property,0,4,False,XA_CARDINAL,&actual,&format,&count,&after,&data)==Success&&data&&format==32&&count>=4){auto*v=reinterpret_cast<unsigned long*>(data);info.userArea={int(v[0]),int(v[1]),int(v[2]),int(v[3])};}if(data)XFree(data);displays_.push_back(info);XCloseDisplay(d);}
#endif
}
const DisplayInfo*Displays::getDisplayForRect(Rectangle<int>r)const{const DisplayInfo*best=getPrimaryDisplay();std::int64_t area=-1;for(const auto&d:displays_){auto overlap=r.getIntersection(d.totalArea);const auto a=std::int64_t(overlap.getWidth())*overlap.getHeight();if(a>area){area=a;best=&d;}}return best;}
Rectangle<int>Displays::getTotalBounds(bool user)const{Rectangle<int>result;for(const auto&d:displays_)result=result.getUnion(user?d.userArea:d.totalArea);return result;}
Desktop&Desktop::getInstance(){static thread_local Desktop desktop;return desktop;}
void Desktop::addFocusChangeListener(FocusChangeListener*l){if(l&&std::find(listeners_.begin(),listeners_.end(),l)==listeners_.end())listeners_.push_back(l);}
void Desktop::removeFocusChangeListener(FocusChangeListener*l){std::erase(listeners_,l);}
void Desktop::notifyFocus(Component*c){const auto listeners=listeners_;for(auto*l:listeners)if(std::find(listeners_.begin(),listeners_.end(),l)!=listeners_.end())l->globalFocusChanged(c);}
Point<int>Desktop::getMousePosition()const{return detail::inputState().mouse.toInt();}
CallOutBox::CallOutBox(std::unique_ptr<Component>content,Component&root,Rectangle<int>anchor):content_(std::move(content)){
 setBounds(root.getLocalBounds());const int w=std::min(getWidth(),content_->getWidth()+20),h=std::min(getHeight(),content_->getHeight()+20);
 panel_={std::clamp(anchor.getCentreX()-w/2,0,std::max(0,getWidth()-w)),anchor.getBottom()+h<=getHeight()?anchor.getBottom():std::max(0,anchor.getY()-h),w,h};
 addAndMakeVisible(*content_);content_->setBounds(panel_.reduced(10));setWantsKeyboardFocus(true);
}
CallOutBox&CallOutBox::launchAsynchronously(std::unique_ptr<Component>content,Rectangle<int>anchor,Component*owner){if(!owner||!owner->getWidgetHost())throw std::logic_error("Native popovers require an attached editor");auto*root=owner->getTopLevelComponent();const auto p=root->getLocalPoint(owner,anchor.getTopLeft());auto box=std::make_unique<CallOutBox>(std::move(content),*root,Rectangle<int>{p.x,p.y,anchor.getWidth(),anchor.getHeight()});auto*result=box.get();owner->getWidgetHost()->showOverlay(std::move(box));return *result;}
void CallOutBox::dismiss(){if(auto*h=getWidgetHost())h->dismissOverlay();}
void CallOutBox::paint(Graphics&g){g.setColour(Colour{0x50000000});g.fillAll();g.setColour(Colour{0xff202629});g.fillRoundedRectangle(panel_.toFloat(),7);g.setColour(Colour{0xff52605b});g.drawRoundedRectangle(panel_.toFloat(),7,1);}
void CallOutBox::mouseDown(const MouseEvent&e){if(!panel_.contains(e.getPosition()))dismiss();}
bool CallOutBox::keyPressed(const KeyPress&k){if(k.isKeyCode(KeyPress::escapeKey)){dismiss();return true;}return false;}
namespace {class Tip:public Component {Text text_;TextLayout layout_;public:Tip(Text text):text_(std::move(text)){setInterceptsMouseClicks(false,false);AttributedString a(text_);a.setFont(Font{12});layout_.createLayout(a,460);setSize(480,int(layout_.getHeight())+20);}void paint(Graphics&g)override{g.setColour(Colour{0xff263029});g.fillRoundedRectangle(getLocalBounds().toFloat(),5);g.setColour(Colour{0xffdfe6e4});layout_.draw(g,getLocalBounds().toFloat().reduced(10));}};}
TooltipWindow::TooltipWindow(Component*p,int delay):parent_(p),delay_(std::max(0,delay)){startTimer(50);}
TooltipWindow::~TooltipWindow()=default;
void TooltipWindow::timerCallback(){auto*p=parent_.getComponent();if(!p||!p->isShowing()){tip_.reset();return;}auto*host=p->getWidgetHost();if(!host)return;auto*hit=p->getTopLevelComponent();auto at=host->mousePosition();std::function<Component*(Component*)>find=[&](Component*c)->Component*{if(!c->isShowing()||!c->getLocalBounds().toFloat().contains(c->getLocalPoint(p->getTopLevelComponent(),Point<float>{float(at.x),float(at.y)})))return nullptr;for(auto i=c->getChildren().rbegin();i!=c->getChildren().rend();++i)if(auto*found=find(*i))return found;return c->getTooltip().isEmpty()?nullptr:c;};hit=find(hit);const auto text=hit?hit->getTooltip():Text{};const Point<int>point{int(at.x),int(at.y)};const auto now=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();if(text!=text_||point!=at_||ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown()){tip_.reset();text_=text;at_=point;since_=now;return;}if(!tip_&&text.isNotEmpty()&&now-since_>=delay_){auto next=std::make_unique<Tip>(text);auto*root=p->getTopLevelComponent();next->setBounds(Rectangle<int>{point.x+12,point.y+20,next->getWidth(),next->getHeight()}.constrainedWithin(root->getLocalBounds()));root->addAndMakeVisible(*next);tip_=std::move(next);}}
class DocumentWindow::Host:public WidgetHost {DocumentWindow&owner_;public:explicit Host(DocumentWindow&o):WidgetHost(o),owner_(o){}const char*title()const noexcept override{return owner_.getName().toRawUTF8();}cell::ui::Point preferredSize()const noexcept override{return {double(owner_.getWidth()),double(owner_.getHeight())};}cell::ui::Point constrainSize(cell::ui::Point p)const noexcept override{return {std::clamp(p.x,double(owner_.minW_),double(owner_.maxW_)),std::clamp(p.y,double(owner_.minH_),double(owner_.maxH_))};}};
DocumentWindow::DocumentWindow(Text name,Colour background,int,bool desktop):Component(std::move(name)),background_(background){desktop_=desktop;setWantsKeyboardFocus(true);}
DocumentWindow::~DocumentWindow(){stopTimer();if(window_)window_->close();window_.reset();host_.reset();clearContentComponent();}
void DocumentWindow::setContentOwned(Component*c,bool fit){setContentNonOwned(c,fit);owned_.reset(c);}
void DocumentWindow::setContentNonOwned(Component*c,bool fit){clearContentComponent();content_=c;if(c){addAndMakeVisible(*c);if(fit)setSize(c->getWidth(),c->getHeight());resized();}}
void DocumentWindow::clearContentComponent(){if(content_)removeChildComponent(content_);content_=nullptr;owned_.reset();}
void DocumentWindow::ensureWindow(){if(!desktop_||!isVisible()||getWidth()<1||getHeight()<1)return;if(!host_)host_=std::make_unique<Host>(*this);if(!window_)window_=std::make_unique<Window>(*host_);if(!window_->isOpen()){if(!window_->open(0,getWidth(),getHeight()))throw std::runtime_error("Cannot create native Cell window");window_->setPosition(getX(),getY());}window_->show(true);startTimer(16);}
void DocumentWindow::setUsingNativeTitleBar(bool v){if(!v)throw std::invalid_argument("Cell document windows currently require the OS title bar");}
void DocumentWindow::setResizable(bool value,bool){resizable_=value;if(window_)window_->setSizeLimits(value?minW_:getWidth(),value?minH_:getHeight(),value?maxW_:getWidth(),value?maxH_:getHeight());}
void DocumentWindow::setResizeLimits(int a,int b,int c,int d){if(a<1||b<1||c<a||d<b)throw std::invalid_argument("Invalid native window size limits");minW_=a;minH_=b;maxW_=c;maxH_=d;if(window_)setResizable(resizable_,false);}
void DocumentWindow::centreAroundComponent(Component*c,int w,int h){if(c){const auto b=c->getScreenBounds();setBounds(b.getCentreX()-w/2,b.getCentreY()-h/2,w,h);}else centreWithSize(w,h);}
void DocumentWindow::centreWithSize(int w,int h){const auto*display=Desktop::getInstance().getDisplays().getPrimaryDisplay();if(!display)throw std::runtime_error("No native display available");const auto b=display->userArea;setBounds(b.getCentreX()-w/2,b.getCentreY()-h/2,w,h);}
void DocumentWindow::addToDesktop(int){desktop_=true;ensureWindow();}void DocumentWindow::removeFromDesktop(){desktop_=false;if(window_)window_->close();stopTimer();}
void DocumentWindow::resized(){if(content_)content_->setBounds(getLocalBounds());if(window_&&window_->isOpen())window_->resize(getWidth(),getHeight());}
void DocumentWindow::visibilityChanged(){if(isVisible())ensureWindow();else if(window_){window_->show(false);stopTimer();}}
void DocumentWindow::paint(Graphics&g){g.fillAll(background_);}
void DocumentWindow::timerCallback(){SafePointer<DocumentWindow>safe(this);Dispatcher::dispatch();if(!safe)return;if(window_){window_->poll();if(safe&&!window_->isOpen()&&isVisible())closeButtonPressed();}}
std::uintptr_t DocumentWindow::nativeHandle()const{return window_?window_->nativeHandle():0;}
void TextLayout::createLayout(const AttributedString&a,float width){font_=a.font;width_=std::max(1.f,width);lines_.clear();Text line;int lastSpace=-1;for(int i=0;i<a.text.length();++i){auto ch=a.text.substring(i,i+1);if(ch=="\n"){lines_.push_back(line);line={};lastSpace=-1;continue;}line+=ch;if(ch==" "||ch=="\t")lastSpace=line.length()-1;if(font_.getStringWidthFloat(line)>width_&&line.length()>1){if(lastSpace>=0){lines_.push_back(line.substring(0,lastSpace));line=line.substring(lastSpace+1);}else{lines_.push_back(line.substring(0,line.length()-1));line=ch;}lastSpace=-1;}}lines_.push_back(line);}
void TextLayout::draw(Graphics&g,Rectangle<float>bounds)const{g.setFont(font_);for(const auto&line:lines_){g.drawText(line,bounds.removeFromTop(font_.getHeight()*1.25f),Justification::centredLeft,false);if(bounds.isEmpty())break;}}
}
namespace cell::ui::widgets {
ColourSelector::ColourSelector(){for(auto*s:{&red_,&green_,&blue_,&alpha_}){addAndMakeVisible(*s);s->setRange(0,255,1);s->setTextBoxStyle(Slider::TextBoxRight,false,60,26);s->onValueChange=[this]{changed();};}addAndMakeVisible(hex_);hex_.onReturnKey=[this]{auto text=hex_.getText().trim().replace("#","");if(text.length()!=6&&text.length()!=8)return;std::uint32_t value{};auto raw=text.utf8();auto result=std::from_chars(raw.data(),raw.data()+raw.size(),value,16);if(result.ec==std::errc{}&&result.ptr==raw.data()+raw.size())setCurrentColour(Colour{raw.size()==6?value|0xff000000u:value});};setSize(290,350);setCurrentColour(Colour{0xffa8d989},dontSendNotification);}
void ColourSelector::setCurrentColour(Colour c,NotificationType notify){if(updating_)return;updating_=true;const auto value=c.getARGB();colour_=c;red_.setValue((value>>16)&255,dontSendNotification);green_.setValue((value>>8)&255,dontSendNotification);blue_.setValue(value&255,dontSendNotification);alpha_.setValue((value>>24)&255,dontSendNotification);char text[10];std::snprintf(text,sizeof(text),"%08x",value);hex_.setText(text,false);updating_=false;repaint();if(notify!=dontSendNotification)sendSynchronousChangeMessage();}
void ColourSelector::changed(){if(!updating_)setCurrentColour(Colour{(std::uint32_t(alpha_.getValue())<<24)|(std::uint32_t(red_.getValue())<<16)|(std::uint32_t(green_.getValue())<<8)|std::uint32_t(blue_.getValue())});}
void ColourSelector::paint(Graphics&g){g.setColour(colour_);g.fillRoundedRectangle(12,12,float(getWidth()-24),70,5);g.setColour(Colour{0xffdfe6e4});g.setFont(Font{12});int y=100;for(auto*name:{"Red","Green","Blue","Alpha"}){g.drawText(name,12,y,48,30,Justification::centredLeft);y+=46;}}
void ColourSelector::resized(){int y=100;for(auto*s:{&red_,&green_,&blue_,&alpha_}){s->setBounds(66,y,getWidth()-78,30);y+=46;}hex_.setBounds(66,y,getWidth()-78,28);}
class AlertWindow::Body final:public Component {
 public:struct Field{Text id,caption;std::unique_ptr<TextEditor>editor;};struct Action{int id;std::unique_ptr<TextButton>button;};
 Text message;TextLayout layout;std::vector<Field>fields;std::vector<Action>actions;AlertWindow&owner;
 Body(AlertWindow&o,Text m):message(std::move(m)),owner(o){AttributedString a(message);a.setFont(Font{13});layout.createLayout(a,500);setSize(548,int(layout.getHeight())+92);}
 void arrange(){setSize(548,int(layout.getHeight())+92+int(fields.size())*58);if(auto*p=getParentComponent())p->setSize(getWidth(),getHeight());resized();}
 void paint(Graphics&g)override{g.setColour(Colour{0xffdfe6e4});layout.draw(g,{24,18,float(getWidth()-48),layout.getHeight()+4});int y=int(layout.getHeight())+36;g.setFont(Font{11});for(const auto&f:fields){g.drawText(f.caption,24,y,getWidth()-48,18,Justification::centredLeft);y+=58;}}
 void resized()override{int y=int(layout.getHeight())+55;for(auto&f:fields){f.editor->setBounds(24,y,getWidth()-48,28);y+=58;}int x=getWidth()-24;for(auto i=actions.rbegin();i!=actions.rend();++i){x-=100;i->button->setBounds(x,getHeight()-48,92,30);}}
};
AlertWindow::AlertWindow(Text title,Text message,MessageBoxIconType):DocumentWindow(std::move(title),Colour{0xff202629},closeButton),body_(std::make_unique<Body>(*this,std::move(message))){setContentNonOwned(body_.get(),true);}
AlertWindow::~AlertWindow(){clearContentComponent();}
void AlertWindow::addTextEditor(Text id,Text initial,Text caption,bool password){if(password)throw std::invalid_argument("Native parameter prompts do not accept passwords");auto editor=std::make_unique<TextEditor>();editor->setText(std::move(initial),false);editor->onReturnKey=[this]{finish(1);};body_->addAndMakeVisible(*editor);body_->fields.push_back({std::move(id),std::move(caption),std::move(editor)});body_->arrange();}
TextEditor*AlertWindow::getTextEditor(Text id)const{for(const auto&f:body_->fields)if(f.id==id)return f.editor.get();return nullptr;}
Text AlertWindow::getTextEditorContents(Text id)const{if(auto*e=getTextEditor(std::move(id)))return e->getText();return {};}
void AlertWindow::addButton(Text caption,int id){auto button=std::make_unique<TextButton>(std::move(caption));button->onClick=[this,id]{finish(id);};body_->addAndMakeVisible(*button);body_->actions.push_back({id,std::move(button)});body_->resized();}
void AlertWindow::enterModalState(bool focus,std::function<void(int)>callback,bool deleteAfter){callback_=std::move(callback);deleteAfter_=deleteAfter;centreAroundComponent(Component::getCurrentlyFocusedComponent(),getWidth(),getHeight());setVisible(true);if(focus&&!body_->fields.empty())body_->fields.front().editor->grabKeyboardFocus();}
void AlertWindow::finish(int result){if(finished_)return;finished_=true;SafePointer<AlertWindow>safe(this);auto callback=std::move(callback_);const bool destroy=deleteAfter_;setVisible(false);if(callback)callback(result);if(destroy)::cell::MainThreadDispatcher::post([safe]{if(safe)delete safe.getComponent();});}
bool AlertWindow::keyPressed(const KeyPress&key){if(key.isKeyCode(KeyPress::escapeKey)){finish(0);return true;}return false;}
}
