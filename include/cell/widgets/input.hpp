// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/widgets/geometry.hpp>
#include <cell/widgets/text.hpp>
#include <functional>
#include <memory>
#include <set>

namespace cell::ui::widgets {
class Component;
enum NotificationType {dontSendNotification=0,sendNotification=1,sendNotificationSync=2,sendNotificationAsync=3};
struct ModifierKeys {
    enum Flags {shiftModifier=1,ctrlModifier=2,altModifier=4,commandModifier=8,leftButtonModifier=16,rightButtonModifier=32,middleButtonModifier=64};
    int flags{};
    ModifierKeys()=default;explicit ModifierKeys(int f):flags(f){}
    bool isShiftDown()const{return flags&shiftModifier;}bool isCtrlDown()const{return flags&ctrlModifier;}
    bool isAltDown()const{return flags&altModifier;}bool isCommandDown()const{return flags&(commandModifier|ctrlModifier);}
    bool isLeftButtonDown()const{return flags&leftButtonModifier;}bool isRightButtonDown()const{return flags&rightButtonModifier;}
    bool isMiddleButtonDown()const{return flags&middleButtonModifier;}
    bool isAnyMouseButtonDown()const{return flags&(leftButtonModifier|rightButtonModifier|middleButtonModifier);}
    bool isAnyModifierKeyDown()const{return flags&(shiftModifier|ctrlModifier|altModifier|commandModifier);}
    bool isPopupMenu()const{return isRightButtonDown();}
    int getRawFlags()const{return flags;}
    bool operator==(const ModifierKeys&)const=default;
    ModifierKeys withoutMouseButtons()const{return ModifierKeys{flags&15};}
    static ModifierKeys getCurrentModifiers()noexcept;
    static ModifierKeys getCurrentModifiersRealtime()noexcept{return getCurrentModifiers();}
};
struct KeyPress {
    enum Key {spaceKey=32,returnKey=13,escapeKey=27,tabKey=9,backspaceKey=8,deleteKey=127,
              leftKey=1002,rightKey=1003,upKey=1000,downKey=1001,homeKey=1006,endKey=1007,
              pageUpKey=1010,pageDownKey=1011,F1Key=1101,F11Key=1111};
    int code{};ModifierKeys modifiers;char32_t character{};
    KeyPress()=default;KeyPress(int c):code(c),character(c>=32&&c<127?char32_t(c):0){}
    KeyPress(int c,ModifierKeys m,char32_t t=0):code(c),modifiers(m),character(t){}
    KeyPress(int c,int m,char32_t t=0):KeyPress(c,ModifierKeys{m},t){}
    bool isKeyCode(int key)const{return code==key;}
    int getKeyCode()const{return code;}ModifierKeys getModifiers()const{return modifiers;}char32_t getTextCharacter()const{return character;}
    bool operator==(const KeyPress&)const=default;
    static bool isKeyCurrentlyDown(int)noexcept;
};
struct MouseInputSource {bool isTouch()const{return false;}};
struct MouseEvent {
    MouseInputSource source;Point<float>position,mouseDownPosition;ModifierKeys mods;
    int x{},y{},clicks=1;Component*eventComponent{};Component*originalComponent{};
    bool dragged{};
    MouseEvent()=default;
    MouseEvent(Component* c,Point<float> p,Point<float> down,ModifierKeys m,int count=1,bool drag=false)
        :position(p),mouseDownPosition(down),mods(m),x(int(p.x)),y(int(p.y)),clicks(count),eventComponent(c),originalComponent(c),dragged(drag){}
    Point<int>getPosition()const{return {x,y};}int getNumberOfClicks()const{return clicks;}
    int getDistanceFromDragStartX()const{return int(position.x-mouseDownPosition.x);}
    int getDistanceFromDragStartY()const{return int(position.y-mouseDownPosition.y);}
    int getDistanceFromDragStart()const{return int(position.getDistanceFrom(mouseDownPosition));}
    Point<int>getOffsetFromDragStart()const{return {getDistanceFromDragStartX(),getDistanceFromDragStartY()};}
    bool mouseWasDraggedSinceMouseDown()const{return dragged||getDistanceFromDragStart()>3;}
    MouseEvent getEventRelativeTo(Component*)const;
    Point<int>getScreenPosition()const;int getScreenX()const{return getScreenPosition().x;}int getScreenY()const{return getScreenPosition().y;}
    MouseEvent withNewPosition(Point<float>p)const{auto e=*this;e.position=p;e.x=int(p.x);e.y=int(p.y);return e;}
};
struct MouseWheelDetails {float deltaX{},deltaY{};bool isReversed{},isSmooth{},isInertial{};};
struct MouseListener {
    virtual~MouseListener()=default;
    virtual void mouseDown(const MouseEvent&){}virtual void mouseDrag(const MouseEvent&){}virtual void mouseUp(const MouseEvent&){}
    virtual void mouseDoubleClick(const MouseEvent&){}virtual void mouseMove(const MouseEvent&){}
    virtual void mouseEnter(const MouseEvent&){}virtual void mouseExit(const MouseEvent&){}
    virtual void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&){}
};
class MouseCursor {
  public:
    enum StandardCursorType {NormalCursor,NoCursor,WaitCursor,IBeamCursor,CrosshairCursor,CopyingCursor,
        PointingHandCursor,DraggingHandCursor,LeftRightResizeCursor,UpDownResizeCursor,
        UpDownLeftRightResizeCursor,TopLeftCornerResizeCursor,TopRightCornerResizeCursor,
        BottomLeftCornerResizeCursor,BottomRightCornerResizeCursor};
    StandardCursorType type=NormalCursor;
    MouseCursor()=default;MouseCursor(StandardCursorType t):type(t){}
    bool operator==(StandardCursorType t)const{return type==t;}
    bool operator==(const MouseCursor&)const=default;
};
struct KeyListener {virtual~KeyListener()=default;virtual bool keyPressed(const KeyPress&,Component*)=0;};
class Timer {
    friend struct TimerRegistry;
    struct State;std::shared_ptr<State>state_;
  public:
    Timer();virtual~Timer();Timer(const Timer&)=delete;Timer&operator=(const Timer&)=delete;
    void startTimer(int milliseconds);void startTimerHz(int hz);void stopTimer();bool isTimerRunning()const;
    virtual void timerCallback()=0;
    static void callAfterDelay(int,std::function<void()>);
    static void dispatchPending(); // UI thread; one shared schedule across views
};
namespace detail {
struct InputState {ModifierKeys modifiers;std::set<int>down;Point<float>mouse;};
InputState&inputState()noexcept;
}
}
