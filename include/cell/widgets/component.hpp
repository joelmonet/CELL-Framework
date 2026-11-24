// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/widgets/drawing.hpp>
#include <cell/widgets/input.hpp>
#include <atomic>
#include <functional>
#include <map>
#include <variant>

namespace cell::ui::widgets {
class Component;class WidgetHost;class LookAndFeel;class Button;class TextButton;class ToggleButton;
class ComboBox;class Label;class Slider;class TableHeaderComponent;
enum class SliderStyle {linearHorizontal,linearVertical,rotary,incDecButtons};
class LookAndFeel {
    std::map<int,Colour>colors_;
  public:
    LookAndFeel();
    virtual~LookAndFeel()=default;
    static LookAndFeel&getDefaultLookAndFeel();
    void setColour(int id,Colour c){colors_[id]=c;}Colour findColour(int id)const{auto i=colors_.find(id);return i==colors_.end()?Colour{0xffdfe6e4}:i->second;}
    virtual void drawButtonBackground(Graphics&,Button&,const Colour&,bool,bool);
    virtual Font getTextButtonFont(TextButton&,int);virtual void drawButtonText(Graphics&,TextButton&,bool,bool);
    virtual Font getComboBoxFont(ComboBox&);virtual Font getPopupMenuFont();
    virtual void drawComboBox(Graphics&,int,int,bool,int,int,int,int,ComboBox&);
    virtual void positionComboBoxText(ComboBox&,Label&);
    virtual Label*createSliderTextBox(Slider&);virtual void drawLabel(Graphics&,Label&);
    virtual void drawToggleButton(Graphics&,ToggleButton&,bool,bool);
    virtual void drawTableHeaderBackground(Graphics&,TableHeaderComponent&);
    virtual void drawTableHeaderColumn(Graphics&,TableHeaderComponent&,const Text&,int,int,int,bool,bool,int);
    virtual void drawLinearSlider(Graphics&,int,int,int,int,float,float,float,SliderStyle,Slider&);
};
class LookAndFeel_V4:public LookAndFeel {};
struct WidgetContext;
class Value {
    std::variant<std::monostate,bool,std::int64_t,double,Text>value_;
  public:
    Value()=default;Value(bool v):value_(v){}Value(int v):value_(std::int64_t(v)){}Value(std::int64_t v):value_(v){}
    template<class T>requires(std::is_integral_v<T>&&!std::is_same_v<T,bool>&&!std::is_same_v<T,int>&&!std::is_same_v<T,std::int64_t>)Value(T v):value_(std::int64_t(v)){}
    Value(double v):value_(v){}Value(float v):value_(double(v)){}Value(Text v):value_(std::move(v)){}Value(const char*v):value_(Text{v}){}
    explicit operator bool()const{return std::visit([](const auto&v)->bool{using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::monostate>)return false;else if constexpr(std::is_same_v<T,Text>)return v.isNotEmpty();else return v!=0;},value_);}
    explicit operator int()const{return int(number());}explicit operator double()const{return number();}
    double number()const{return std::visit([](const auto&v)->double{using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::monostate>)return 0;else if constexpr(std::is_same_v<T,Text>)return v.getDoubleValue();else return double(v);},value_);}
    Text toString()const{return std::visit([](const auto&v)->Text{using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::monostate>)return {};else if constexpr(std::is_same_v<T,Text>)return v;else if constexpr(std::is_same_v<T,bool>)return v?"1":"0";else if constexpr(std::is_integral_v<T>)return Text{v};else return Text{v,8};},value_);}
};
class Properties {
    std::map<Text,Value>values_;
  public:
    void set(Text key,Value v){values_[std::move(key)]=std::move(v);}
    bool contains(const Text&key)const{return values_.contains(key);}
    Value operator[](const Text&key)const{auto i=values_.find(key);return i==values_.end()?Value{}:i->second;}
    Value getWithDefault(const Text&key,Value fallback)const{auto i=values_.find(key);return i==values_.end()?fallback:i->second;}
    void remove(const Text&key){values_.erase(key);}
};
struct SettableTooltipClient {};
class Component:public MouseListener {
    friend class WidgetHost;friend struct WidgetContext;
    struct Lifetime {std::atomic<Component*>pointer{};};
    std::shared_ptr<Lifetime>lifetime_=std::make_shared<Lifetime>();
    Rectangle<int>bounds_;
    AffineTransform transform_;
    Component*parent_{};std::vector<Component*>children_;
    using PaintChildren=std::vector<std::weak_ptr<Lifetime>>;
    std::shared_ptr<const PaintChildren>paintChildren_;
    void rebuildPaintOrder();
    WidgetContext*context_{};
    bool visible_{},enabled_=true,opaque_{},focusable_{},interceptSelf_=true,interceptChildren_=true;
    float alpha_=1;
    Text id_,name_,tooltip_;
    Properties properties_;
    std::map<int,Colour>colors_;
    LookAndFeel*look_{};MouseCursor cursor_;
    std::vector<KeyListener*>keyListeners_;
    struct Listener {MouseListener*listener;bool recursive;};std::vector<Listener>listeners_;
    void attachContext(WidgetContext*);
  public:
    template<class T>class SafePointer {
        std::weak_ptr<Lifetime>life_;
      public:
        SafePointer()=default;SafePointer(std::nullptr_t){}SafePointer(T*p){if(p)life_=p->lifetime_;}
        T*getComponent()const noexcept{auto p=life_.lock();return p?static_cast<T*>(p->pointer.load()):nullptr;}
        T*operator->()const noexcept{return getComponent();}operator T*()const noexcept{return getComponent();}
        bool operator==(std::nullptr_t)const noexcept{return !getComponent();}
        bool operator!=(std::nullptr_t)const noexcept{return bool(getComponent());}
    };
    Component();explicit Component(Text name);virtual~Component();Component(const Component&)=delete;Component&operator=(const Component&)=delete;
    void setBounds(int,int,int,int);void setBounds(Rectangle<int>r){setBounds(r.getX(),r.getY(),r.getWidth(),r.getHeight());}
    virtual void setSize(int w,int h){setBounds(getX(),getY(),w,h);}void setTopLeftPosition(int x,int y){setBounds(x,y,getWidth(),getHeight());}
    void setCentrePosition(int x,int y){setTopLeftPosition(x-getWidth()/2,y-getHeight()/2);}
    Rectangle<int>getBounds()const{return bounds_;}Rectangle<int>getLocalBounds()const{return {0,0,getWidth(),getHeight()};}
    int getX()const{return bounds_.getX();}int getY()const{return bounds_.getY();}int getWidth()const{return bounds_.getWidth();}int getHeight()const{return bounds_.getHeight();}
    int getRight()const{return bounds_.getRight();}int getBottom()const{return bounds_.getBottom();}
    void setVisible(bool);bool isVisible()const{return visible_;}bool isShowing()const;
    void setEnabled(bool);bool isEnabled()const{return enabled_&&(!parent_||parent_->isEnabled());}
    void setOpaque(bool v){opaque_=v;}bool isOpaque()const{return opaque_;}
    void setAlpha(float a){alpha_=std::clamp(a,0.f,1.f);repaint();}float getAlpha()const{return alpha_;}
    void setTransform(AffineTransform t){transform_=t;repaint();}AffineTransform getTransform()const{return transform_;}
    void addChildComponent(Component&,int index=-1);void addChildComponent(Component*c,int index=-1){if(c)addChildComponent(*c,index);}
    void addAndMakeVisible(Component&c,int index=-1){addChildComponent(c,index);c.setVisible(true);}void addAndMakeVisible(Component*c,int index=-1){if(c)addAndMakeVisible(*c,index);}
    void removeChildComponent(Component*);void removeAllChildren();void deleteAllChildren();
    int getNumChildComponents()const{return int(children_.size());}Component*getChildComponent(int i)const{return i>=0&&std::size_t(i)<children_.size()?children_[std::size_t(i)]:nullptr;}
    const std::vector<Component*>&getChildren()const{return children_;}
    Component*getParentComponent()const{return parent_;}Component*getTopLevelComponent()noexcept;const Component*getTopLevelComponent()const noexcept;
    bool isParentOf(const Component*)const;template<class T>T*findParentComponentOfClass()const{for(auto*p=parent_;p;p=p->parent_)if(auto*match=dynamic_cast<T*>(p))return match;return nullptr;}
    void toFront(bool focus);void toBack();
    void setComponentID(Text v){id_=std::move(v);}const Text&getComponentID()const{return id_;}
    void setName(Text v){name_=std::move(v);}const Text&getName()const{return name_;}
    void setTitle(Text v){properties_.set("title",Value{std::move(v)});}void setDescription(Text v){properties_.set("description",Value{std::move(v)});}
    Properties&getProperties(){return properties_;}const Properties&getProperties()const{return properties_;}
    void setTooltip(Text v){tooltip_=std::move(v);}const Text&getTooltip()const{return tooltip_;}
    void setWantsKeyboardFocus(bool v){focusable_=v;}bool getWantsKeyboardFocus()const{return focusable_;}
    void grabKeyboardFocus();bool hasKeyboardFocus(bool children)const;static Component*getCurrentlyFocusedComponent()noexcept;
    void setMouseClickGrabsKeyboardFocus(bool v){focusable_=v;}
    void setInterceptsMouseClicks(bool self,bool children){interceptSelf_=self;interceptChildren_=children;}
    void setMouseCursor(MouseCursor c){cursor_=c;}MouseCursor getMouseCursor()const{return cursor_;}
    bool isMouseOver(bool children=false)const;bool isMouseButtonDown()const;bool isMouseOverOrDragging()const{return isMouseOver(true)||isMouseButtonDown();}
    Point<int>getMouseXYRelative()const;Point<float>getLocalPoint(const Component*,Point<float>)const;Point<int>getLocalPoint(const Component*c,Point<int>p)const{return getLocalPoint(c,p.toFloat()).toInt();}
    Rectangle<int>getLocalArea(const Component*c,Rectangle<int>r)const{return r.transformedBy(c?c->localToRoot().followedBy(localToRoot().inverted()):localToRoot().inverted());}
    Point<int>localPointToGlobal(Point<int>)const;Point<int>getScreenPosition()const{return localPointToGlobal({0,0});}
    Rectangle<int>getScreenBounds()const{const auto p=getScreenPosition();return {p.x,p.y,getWidth(),getHeight()};}
    static float getApproximateScaleFactorForComponent(const Component*);
    AffineTransform localToRoot()const;
    void setColour(int id,Colour v){colors_[id]=v;repaint();}Colour findColour(int,bool inherit=true)const;
    void setLookAndFeel(LookAndFeel*);LookAndFeel&getLookAndFeel()const;
    void addKeyListener(KeyListener*l){if(l&&std::find(keyListeners_.begin(),keyListeners_.end(),l)==keyListeners_.end())keyListeners_.push_back(l);}
    void removeKeyListener(KeyListener*l){std::erase(keyListeners_,l);}
    void addMouseListener(MouseListener*,bool);void removeMouseListener(MouseListener*);
    void repaint();void repaint(int x,int y,int w,int h){repaint(Rectangle<int>{x,y,w,h});}void repaint(Rectangle<int>);
    WidgetHost*getWidgetHost()const noexcept;
    void paintEntireComponent(Graphics&,bool ignoreAlpha=false);Image createComponentSnapshot(Rectangle<int>,bool clip=true,float scale=1);
    virtual void paint(Graphics&){}virtual void paintOverChildren(Graphics&){}
    virtual void resized(){}virtual void moved(){}virtual void visibilityChanged(){}virtual void enablementChanged(){}virtual void parentHierarchyChanged(){}
    virtual void childBoundsChanged(Component*){}virtual void lookAndFeelChanged(){}
    virtual bool hitTest(int x,int y){return getLocalBounds().contains(x,y);}
    virtual bool keyPressed(const KeyPress&){return false;}virtual bool keyStateChanged(bool){return false;}
    enum FocusChangeType{focusChangedDirectly,focusChangedByMouseClick,focusChangedByTabKey};
    virtual void mouseCaptureLost(){}virtual void focusGained(){}virtual void focusLost(){}
    virtual void focusGained(FocusChangeType){focusGained();}virtual void focusLost(FocusChangeType){focusLost();}
    void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&)override;
};
class WidgetHost:public cell::ui::Editor {
    struct Impl;std::shared_ptr<Impl>impl_;
  public:
    explicit WidgetHost(Component&);~WidgetHost()override;
    void paint(Canvas&,cell::ui::Rect)override;bool pointer(const cell::ui::PointerEvent&)override;
    bool keyboard(const cell::ui::KeyEvent&)override;bool keyReleased(const cell::ui::KeyEvent&)override;
    void cancelInput()override;bool tick()override;void visibility(bool)override;
    int pointerCursor()const noexcept override;
    bool preserveAspectRatio()const noexcept override{return false;}
    Point<double>mousePosition()const noexcept;
    void setScreenOrigin(Point<int>);
    void invalidateArea(Rectangle<int>);
    void showOverlay(std::unique_ptr<Component>);
    void dismissOverlay();
};
template<class T>class OwnedArray {
    std::vector<std::unique_ptr<T>>values_;
  public:
    void add(T*p){values_.emplace_back(p);}void clear(){values_.clear();}int size()const{return int(values_.size());}bool isEmpty()const{return values_.empty();}void remove(int index){if(index>=0&&std::size_t(index)<values_.size())values_.erase(values_.begin()+index);}
    T*operator[](int i)const{return values_[std::size_t(i)].get();}
    struct Iterator {typename std::vector<std::unique_ptr<T>>::iterator it;T*operator*()const{return it->get();}Iterator&operator++(){++it;return *this;}bool operator!=(const Iterator&b)const{return it!=b.it;}};
    Iterator begin(){return {values_.begin()};}Iterator end(){return {values_.end()};}
};
}
