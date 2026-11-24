// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/widgets/navigation.hpp>
#include <filesystem>
#include <cell/dispatch.hpp>

namespace cell::ui::widgets {
// Background workers post immutable results; callbacks run only when the UI pumps.
using Dispatcher=::cell::MainThreadDispatcher;

class ListBoxModel {
 public:
 virtual ~ListBoxModel()=default;virtual int getNumRows()=0;
 virtual void paintListBoxItem(int,Graphics&,int,int,bool)=0;
 virtual void selectedRowsChanged(int){}virtual void listBoxItemClicked(int,const MouseEvent&){}
 virtual void listBoxItemDoubleClicked(int,const MouseEvent&){}virtual void returnKeyPressed(int){}
};
class ListBox:public Component,private TableListBoxModel {
 TableListBox table_;ListBoxModel*model_{};
 int getNumRows()override{return model_?model_->getNumRows():0;}
 void paintRowBackground(Graphics&,int,int,int,bool)override{}
 void paintCell(Graphics&g,int row,int,int w,int h,bool selected)override{if(model_)model_->paintListBoxItem(row,g,w,h,selected);}
 void selectedRowsChanged(int row)override{if(model_)model_->selectedRowsChanged(row);}
 void cellClicked(int row,int,const MouseEvent&e)override{if(model_)model_->listBoxItemClicked(row,e.getEventRelativeTo(this));}
 void cellDoubleClicked(int row,int,const MouseEvent&e)override{if(model_)model_->listBoxItemDoubleClicked(row,e.getEventRelativeTo(this));}
 public:
 enum ColourIds{backgroundColourId=0x800,outlineColourId,textColourId};
 ListBox();ListBox(Text name,ListBoxModel*m):ListBox(){setName(std::move(name));setModel(m);}
 void setModel(ListBoxModel*);void updateContent(){table_.updateContent();}
 void setRowHeight(int h){table_.setRowHeight(h);}void setOutlineThickness(int n){outline_=std::max(0,n);resized();}
 void selectRow(int row){table_.selectRow(row);}int getSelectedRow()const{return table_.getSelectedRow();}
 void deselectAllRows(){table_.deselectAllRows();}void scrollToEnsureRowIsOnscreen(int row){table_.scrollToEnsureRowIsOnscreen(row);}
 void paint(Graphics&)override;void resized()override;bool keyPressed(const KeyPress&)override;
 private:int outline_{};
};
struct FocusChangeListener {virtual~FocusChangeListener()=default;virtual void globalFocusChanged(Component*)=0;};
struct DisplayInfo {Rectangle<int>totalArea,userArea;double scale=1;};
class Displays {
 std::vector<DisplayInfo>displays_;
 public:
 Displays();void refresh();const DisplayInfo*getDisplayForRect(Rectangle<int>)const;
 const DisplayInfo*getPrimaryDisplay()const{return displays_.empty()?nullptr:&displays_.front();}
 Rectangle<int>getTotalBounds(bool user)const;
};
class Desktop {
 Displays displays_;std::vector<FocusChangeListener*>listeners_;
 public:
 static Desktop&getInstance();Displays&getDisplays(){return displays_;}
 void addFocusChangeListener(FocusChangeListener*);void removeFocusChangeListener(FocusChangeListener*);
 void notifyFocus(Component*);Point<int>getMousePosition()const;
};
class CallOutBox:public Component {
 std::unique_ptr<Component>content_;Rectangle<int>panel_;
 public:
 CallOutBox(std::unique_ptr<Component>,Component&,Rectangle<int>);
 static CallOutBox&launchAsynchronously(std::unique_ptr<Component>,Rectangle<int>,Component*);
 void dismiss();void paint(Graphics&)override;void mouseDown(const MouseEvent&)override;
 bool keyPressed(const KeyPress&)override;
};
class TooltipWindow:private Timer {
 Component::SafePointer<Component>parent_;int delay_;double since_{};Text text_;Point<int>at_;std::unique_ptr<Component>tip_;
 void timerCallback()override;
 public:TooltipWindow(Component*,int delay=500);~TooltipWindow()override;
};
// Owned native windows keep painting and input in Cell, using the OS title bar.
class DocumentWindow:public Component,private Timer {
 class Host;std::unique_ptr<Host>host_;std::unique_ptr<Window>window_;
 std::unique_ptr<Component>owned_;Component*content_{};Colour background_;
 bool desktop_{},resizable_=true,closing_{};int minW_=1,minH_=1,maxW_=std::numeric_limits<int>::max(),maxH_=std::numeric_limits<int>::max();
 void timerCallback()override;void ensureWindow();
 public:
 enum Buttons{minimiseButton=1,maximiseButton=2,closeButton=4};
 DocumentWindow(Text,Colour,int,bool addToDesktop=true);~DocumentWindow()override;
 void setContentOwned(Component*,bool resizeToFit);void setContentNonOwned(Component*,bool);void clearContentComponent();Component*getContentComponent()const{return content_;}
 void setUsingNativeTitleBar(bool);void setResizable(bool,bool);void setResizeLimits(int,int,int,int);
 void centreAroundComponent(Component*,int,int);void centreWithSize(int,int);
 void addToDesktop(int);void removeFromDesktop();int getDesktopWindowStyleFlags()const{return 0;}
 void resized()override;void visibilityChanged()override;void paint(Graphics&)override;
 virtual void closeButtonPressed(){setVisible(false);}virtual void activeWindowStatusChanged(){}
 bool isActiveWindow()const{return hasKeyboardFocus(true);}std::uintptr_t nativeHandle()const;
};
struct ResizableWindow {enum{backgroundColourId=0x900};};
struct FileBrowserComponent {enum Flags{openMode=1,saveMode=2,canSelectFiles=4,canSelectDirectories=8,canSelectMultipleItems=16,warnAboutOverwriting=32};};
class AttributedString {
 public:Text text;Font font;explicit AttributedString(Text t):text(std::move(t)){}void setFont(Font f){font=f;}
};
class TextLayout {
 std::vector<Text>lines_;Font font_;float width_{};
 public:void createLayout(const AttributedString&,float);float getHeight()const{return float(lines_.size())*font_.getHeight()*1.25f;}
 void draw(Graphics&,Rectangle<float>)const;
};
}
namespace cell::ui::widgets {
class ChangeBroadcaster;
struct ChangeListener {virtual~ChangeListener()=default;virtual void changeListenerCallback(ChangeBroadcaster*)=0;};
class ChangeBroadcaster {
 struct Notifications{std::atomic<ChangeBroadcaster*>owner;std::atomic<bool>pending{false};explicit Notifications(ChangeBroadcaster*p):owner(p){}};
 std::shared_ptr<Notifications>notifications_=std::make_shared<Notifications>(this);
 std::vector<ChangeListener*>listeners_;
 public:virtual~ChangeBroadcaster(){::cell::ControlScope control;notifications_->owner=nullptr;}
 ChangeBroadcaster()=default;ChangeBroadcaster(const ChangeBroadcaster&)=delete;ChangeBroadcaster&operator=(const ChangeBroadcaster&)=delete;
 void sendChangeMessage(){const auto state=notifications_;if(state->pending.exchange(true))return;try{Dispatcher::post([state]{state->pending=false;if(auto*owner=state->owner.load())owner->sendSynchronousChangeMessage();});}catch(...){state->pending=false;throw;}}
 void addChangeListener(ChangeListener*l){if(l&&std::find(listeners_.begin(),listeners_.end(),l)==listeners_.end())listeners_.push_back(l);}
 void removeChangeListener(ChangeListener*l){std::erase(listeners_,l);}
 void sendSynchronousChangeMessage(){const auto list=listeners_;for(auto*l:list)if(std::find(listeners_.begin(),listeners_.end(),l)!=listeners_.end())l->changeListenerCallback(this);}
};
class ColourSelector:public Component,public ChangeBroadcaster {
 Colour colour_;Slider red_,green_,blue_,alpha_;TextEditor hex_;bool updating_{};
 void changed();
 public:ColourSelector();void setCurrentColour(Colour,NotificationType=sendNotification);Colour getCurrentColour()const{return colour_;}
 void paint(Graphics&)override;void resized()override;
};
enum class MessageBoxIconType{NoIcon,QuestionIcon,WarningIcon,InfoIcon};
struct ModalCallbackFunction {template<class Fn>static std::function<void(int)>create(Fn fn){return std::move(fn);}};
class AlertWindow:public DocumentWindow {
 class Body;std::unique_ptr<Body>body_;std::function<void(int)>callback_;bool deleteAfter_{},finished_{};
 void finish(int);
 public:AlertWindow(Text title,Text message,MessageBoxIconType);~AlertWindow()override;
 void addTextEditor(Text id,Text initial,Text caption,bool password=false);
 Text getTextEditorContents(Text id)const;TextEditor*getTextEditor(Text id)const;
 void addButton(Text,int);void enterModalState(bool,std::function<void(int)>,bool deleteAfter);
 void closeButtonPressed()override{finish(0);}bool keyPressed(const KeyPress&)override;
};
}
