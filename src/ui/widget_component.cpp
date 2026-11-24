// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/component.hpp>
#include <cell/widgets/services.hpp>
#include <cmath>

namespace cell::ui::widgets {
namespace {thread_local Component*currentFocus{};}
struct WidgetContext {
    WidgetHost*host;Component::SafePointer<Component>root,focused,captured,hovered;
    Point<float>mouse,down;Point<int>screen;
    bool dirty=true,visible=true;int clicks=1;
    std::unique_ptr<Component>overlay;
    Component::SafePointer<Component>beforeOverlay;
    explicit WidgetContext(WidgetHost&h,Component&r):host(&h),root(&r){}
    void invalidate(){dirty=true;if(host&&host->invalidate)host->invalidate();}
    void focus(Component*c){if(focused.getComponent()==c)return;auto old=focused;focused=c;currentFocus=c;if(old)old->focusLost(Component::focusChangedDirectly);if(focused)focused->focusGained(Component::focusChangedDirectly);Desktop::getInstance().notifyFocus(focused);invalidate();}
    void release(Component*c,bool notify){
        Component::SafePointer<Component>branch(c);
        const auto within=[branch](Component*p){auto*c=branch.getComponent();return c&&p&&(p==c||c->isParentOf(p));};
        if(within(captured)){auto previous=captured;captured=nullptr;if(notify&&previous)previous->mouseCaptureLost();}
        if(within(focused)){auto old=focused;focused=nullptr;if(currentFocus==old.getComponent())currentFocus=nullptr;if(notify&&old)old->focusLost(Component::focusChangedDirectly);}
        if(within(hovered))hovered=nullptr;
    }
    Component*hit(Component*c,Point<float>rootPoint){
        if(!c||!c->isVisible()||!c->isEnabled())return nullptr;
        const auto point=c->localToRoot().inverted().transformedPoint(rootPoint);
        if(!c->hitTest(int(std::floor(point.x)),int(std::floor(point.y))))return nullptr;
        if(c->interceptChildren_)for(auto i=c->children_.rbegin();i!=c->children_.rend();++i)if(auto*found=hit(*i,rootPoint))return found;
        return c->interceptSelf_?c:nullptr;
    }
    MouseEvent event(Component*c,ModifierKeys modifiers,bool drag=false)const{
        const auto transform=c->localToRoot().inverted();return {c,transform.transformedPoint(mouse),transform.transformedPoint(down),modifiers,clicks,drag};
    }
    template<class Fn>void send(Component*c,Fn fn){
        if(!c)return;
        Component::SafePointer<Component>safe(c);
        std::vector<Component::SafePointer<Component>>chain;
        for(auto*p=c;p;p=p->parent_)chain.emplace_back(p);
        fn(*c,event(c,detail::inputState().modifiers));
        if(!safe)return;
        for(const auto&weak:chain){
            auto*owner=weak.getComponent();if(!owner||!safe)return;
            const auto listeners=owner->listeners_;
            for(const auto&item:listeners){
                owner=weak.getComponent();if(!owner||!safe)return;
                const bool registered=std::any_of(owner->listeners_.begin(),owner->listeners_.end(),[&](const auto&i){return i.listener==item.listener;});
                if(registered&&(owner==safe.getComponent()||item.recursive)){
                    auto e=event(owner,detail::inputState().modifiers);e.originalComponent=safe.getComponent();fn(*item.listener,e);
                }
            }
        }
    }

};
Component::Component(){lifetime_->pointer=this;}
void Component::rebuildPaintOrder(){auto next=std::make_shared<PaintChildren>();next->reserve(children_.size());for(auto*c:children_)next->push_back(c->lifetime_);paintChildren_=std::move(next);}
Component::Component(Text name):Component(){name_=std::move(name);}
Component::~Component(){
    if(context_)context_->release(this,false);
    lifetime_->pointer=nullptr;
    if(parent_)parent_->removeChildComponent(this);
    for(auto*child:children_){child->parent_=nullptr;child->attachContext(nullptr);}
}
void Component::attachContext(WidgetContext*c){if(context_==c)return;SafePointer<Component>self(this);auto*previous=context_;context_=c;if(previous)previous->release(this,true);if(!self)return;const auto children=paintChildren_;if(children)for(const auto&weak:*children){auto life=weak.lock();auto*child=life?life->pointer.load():nullptr;if(child&&child->parent_==this)child->attachContext(c);if(!self)return;}}
void Component::setBounds(int x,int y,int w,int h){Rectangle<int>next{x,y,w,h};if(next==bounds_)return;const bool size=w!=getWidth()||h!=getHeight(),position=x!=getX()||y!=getY();bounds_=next;SafePointer<Component>self(this);if(size)resized();if(!self)return;if(position)moved();if(!self)return;if(parent_)parent_->childBoundsChanged(this);if(self)repaint();}
void Component::setVisible(bool v){if(v==visible_)return;SafePointer<Component>self(this);visible_=v;if(!v&&context_)context_->release(this,true);if(!self)return;visibilityChanged();if(self)repaint();}
bool Component::isShowing()const{return visible_&&(!parent_||parent_->isShowing())&&(!context_||context_->visible);}
void Component::setEnabled(bool v){if(enabled_==v)return;SafePointer<Component>self(this);enabled_=v;if(!v&&context_)context_->release(this,true);if(!self)return;enablementChanged();if(self)repaint();}
void Component::addChildComponent(Component&c,int index){if(&c==this||c.isParentOf(this))throw std::invalid_argument("A native widget cannot contain an ancestor");if(c.parent_==this)return;if(c.parent_)c.parent_->removeChildComponent(&c);auto at=index<0?children_.size():std::min(children_.size(),std::size_t(index));children_.insert(children_.begin()+std::ptrdiff_t(at),&c);c.parent_=this;rebuildPaintOrder();SafePointer<Component>self(this),child(&c);c.attachContext(context_);if(child)child->parentHierarchyChanged();if(self)repaint();}
void Component::removeChildComponent(Component*c){auto i=std::find(children_.begin(),children_.end(),c);if(i==children_.end())return;SafePointer<Component>self(this),child(c);auto*context=context_;children_.erase(i);rebuildPaintOrder();c->parent_=nullptr;if(context)context->release(c,true);if(child){child->attachContext(nullptr);if(child)child->parentHierarchyChanged();}if(self)repaint();}
void Component::removeAllChildren(){while(!children_.empty())removeChildComponent(children_.back());}
void Component::deleteAllChildren(){while(!children_.empty()){auto*c=children_.back();removeChildComponent(c);delete c;}}
Component*Component::getTopLevelComponent()noexcept{auto*p=this;while(p->parent_)p=p->parent_;return p;}
const Component*Component::getTopLevelComponent()const noexcept{auto*p=this;while(p->parent_)p=p->parent_;return p;}
bool Component::isParentOf(const Component*c)const{for(auto*p=c?c->parent_:nullptr;p;p=p->parent_)if(p==this)return true;return false;}
void Component::toFront(bool focus){if(parent_){auto&v=parent_->children_;auto i=std::find(v.begin(),v.end(),this);if(i!=v.end()){v.erase(i);v.push_back(this);parent_->rebuildPaintOrder();}}if(focus)grabKeyboardFocus();repaint();}
void Component::toBack(){if(parent_){auto&v=parent_->children_;auto i=std::find(v.begin(),v.end(),this);if(i!=v.end()){v.erase(i);v.insert(v.begin(),this);parent_->rebuildPaintOrder();}}repaint();}
void Component::grabKeyboardFocus(){if(context_&&isEnabled()&&isShowing())context_->focus(this);}
bool Component::hasKeyboardFocus(bool children)const{return context_&&(context_->focused.getComponent()==this||(children&&isParentOf(context_->focused)));}
Component*Component::getCurrentlyFocusedComponent()noexcept{return currentFocus;}
bool Component::isMouseOver(bool children)const{return context_&&(context_->hovered.getComponent()==this||(children&&isParentOf(context_->hovered)));}
bool Component::isMouseButtonDown()const{return context_&&context_->captured.getComponent()==this;}
Point<int>Component::getMouseXYRelative()const{return context_?localToRoot().inverted().transformedPoint(context_->mouse).toInt():Point<int>{};}
AffineTransform Component::localToRoot()const{if(!parent_)return transform_;return transform_.translated(getX(),getY()).followedBy(parent_->localToRoot());}
Point<float>Component::getLocalPoint(const Component*c,Point<float>p)const{if(c)p=c->localToRoot().transformedPoint(p);else if(context_)p=p-context_->screen.toFloat();return localToRoot().inverted().transformedPoint(p);}
Point<int>Component::localPointToGlobal(Point<int>p)const{auto n=localToRoot().transformedPoint(p.toFloat());if(context_)n=n+context_->screen.toFloat();return n.toInt();}
float Component::getApproximateScaleFactorForComponent(const Component*c){if(!c)return 1.f;const auto t=c->localToRoot();const double physical=c->context_&&c->context_->host?c->context_->host->nativeWindowScale:1;return float(physical*std::sqrt(std::max(0.,std::abs(t.mat00*t.mat11-t.mat01*t.mat10))));}
Colour Component::findColour(int id,bool inherit)const{auto i=colors_.find(id);if(i!=colors_.end())return i->second;if(inherit&&parent_)return parent_->findColour(id,true);return getLookAndFeel().findColour(id);}
void Component::setLookAndFeel(LookAndFeel*p){look_=p;lookAndFeelChanged();for(auto*c:children_)c->lookAndFeelChanged();repaint();}
LookAndFeel&Component::getLookAndFeel()const{return look_?*look_:parent_?parent_->getLookAndFeel():LookAndFeel::getDefaultLookAndFeel();}
void Component::addMouseListener(MouseListener*l,bool recursive){if(l&&std::none_of(listeners_.begin(),listeners_.end(),[l](const auto&e){return e.listener==l;}))listeners_.push_back({l,recursive});}
void Component::removeMouseListener(MouseListener*l){std::erase_if(listeners_,[l](const auto&e){return e.listener==l;});}
void Component::repaint(){if(context_)context_->invalidate();}
void Component::repaint(Rectangle<int>r){if(context_&&context_->host)context_->host->invalidateArea(r.transformedBy(localToRoot()));}
WidgetHost*Component::getWidgetHost()const noexcept{return context_?context_->host:nullptr;}
void Component::mouseWheelMove(const MouseEvent&e,const MouseWheelDetails&wheel){if(parent_)parent_->mouseWheelMove(e.getEventRelativeTo(parent_),wheel);}
void Component::paintEntireComponent(Graphics&g,bool ignoreAlpha){
    if(!visible_||getWidth()<1||getHeight()<1)return;
    SafePointer<Component>self(this);Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(getLocalBounds());if(!ignoreAlpha&&alpha_<1)g.multiplyOpacity(alpha_);
    paint(g);if(!self)return;
    const auto children=paintChildren_;
    if(children)for(const auto&weak:*children){auto life=weak.lock();auto*child=life?life->pointer.load():nullptr;
        if(child&&child->parent_==this&&child->visible_){Graphics::ScopedSaveState childState(g);g.addTransform(child->transform_.translated(child->getX(),child->getY()));child->paintEntireComponent(g,ignoreAlpha);if(!self)return;}}
    paintOverChildren(g);
}
Image Component::createComponentSnapshot(Rectangle<int>r,bool,float scale){if(r.isEmpty()||!std::isfinite(scale)||scale<=0)return {};Image result(Image::ARGB,int(std::ceil(r.getWidth()*scale)),int(std::ceil(r.getHeight()*scale)),true);{Graphics g(result);g.addTransform(AffineTransform::translation(-r.getX(),-r.getY()).scaled(scale));paintEntireComponent(g,true);}return result;}
MouseEvent MouseEvent::getEventRelativeTo(Component*c)const{if(!c)return *this;auto event=*this;event.position=c->getLocalPoint(eventComponent,position);event.mouseDownPosition=c->getLocalPoint(eventComponent,mouseDownPosition);event.x=int(event.position.x);event.y=int(event.position.y);event.eventComponent=c;return event;}
Point<int>MouseEvent::getScreenPosition()const{return eventComponent?eventComponent->localPointToGlobal(getPosition()):getPosition();}
struct WidgetHost::Impl {WidgetContext context;explicit Impl(WidgetHost&h,Component&r):context(h,r){r.attachContext(&context);r.setVisible(true);}~Impl(){if(context.root)context.root->attachContext(nullptr);}};
WidgetHost::WidgetHost(Component&r):impl_(std::make_shared<Impl>(*this,r)){}
WidgetHost::~WidgetHost(){impl_->context.host=nullptr;}
void WidgetHost::paint(Canvas&c,cell::ui::Rect bounds){::cell::ControlScope control;auto lifetime=impl_;auto&state=lifetime->context;if(!state.root)return;auto*root=state.root.getComponent();if(root->getWidth()!=int(bounds.width)||root->getHeight()!=int(bounds.height))root->setSize(int(bounds.width),int(bounds.height));if(!state.host||!state.root)return;Graphics g(c,{0,0,float(bounds.width),float(bounds.height)});state.root->paintEntireComponent(g);state.dirty=false;}
bool WidgetHost::pointer(const cell::ui::PointerEvent&e){::cell::ControlScope control;auto lifetime=impl_;auto&c=lifetime->context;if(!c.root)return false;if(e.action==PointerAction::cancel){cancelInput();return true;}c.mouse={float(e.position.x),float(e.position.y)};auto&input=detail::inputState();input.mouse=c.mouse;input.modifiers.flags=(input.modifiers.flags&112)|(e.fine?ModifierKeys::shiftModifier:0)|(e.control?ModifierKeys::ctrlModifier:0)|(e.alt?ModifierKeys::altModifier:0)|(e.command?ModifierKeys::commandModifier:0);auto*target=c.hit(c.root,c.mouse);
    if(e.action==PointerAction::move){if(c.hovered.getComponent()!=target){auto previous=c.hovered;c.hovered=target;if(previous)c.send(previous,[](MouseListener&l,const MouseEvent&m){l.mouseExit(m);});if(target)c.send(target,[](MouseListener&l,const MouseEvent&m){l.mouseEnter(m);});c.invalidate();}if(c.captured)c.send(c.captured,[](MouseListener&l,const MouseEvent&m){l.mouseDrag(m);});else if(target)c.send(target,[](MouseListener&l,const MouseEvent&m){l.mouseMove(m);});return bool(target);}
    if(e.action==PointerAction::down||e.action==PointerAction::context){input.modifiers.flags|=e.action==PointerAction::context?ModifierKeys::rightButtonModifier:e.button==PointerButton::middle?ModifierKeys::middleButtonModifier:ModifierKeys::leftButtonModifier;c.down=c.mouse;c.clicks=e.reset?2:1;c.captured=target;if(target){if(target->focusable_)c.focus(target);c.send(target,[](MouseListener&l,const MouseEvent&m){l.mouseDown(m);});if(e.reset&&c.captured)c.send(c.captured,[](MouseListener&l,const MouseEvent&m){l.mouseDoubleClick(m);});}if(e.action==PointerAction::context){c.captured=nullptr;input.modifiers.flags&=~ModifierKeys::rightButtonModifier;}c.invalidate();return bool(target);}
    if(e.action==PointerAction::up){auto captured=c.captured;c.captured=nullptr;input.modifiers.flags&=~(ModifierKeys::leftButtonModifier|ModifierKeys::middleButtonModifier);if(captured)c.send(captured,[](MouseListener&l,const MouseEvent&m){l.mouseUp(m);});c.invalidate();return bool(captured);}
    if(e.action==PointerAction::wheel&&target){MouseWheelDetails wheel{};wheel.deltaY=float(e.wheel)*.1f;c.send(target,[&](MouseListener&l,const MouseEvent&m){l.mouseWheelMove(m,wheel);});return true;}return false;
}
bool WidgetHost::keyboard(const cell::ui::KeyEvent&e){::cell::ControlScope control;auto lifetime=impl_;auto&c=lifetime->context;auto&state=detail::inputState();state.modifiers=ModifierKeys{(e.shift?ModifierKeys::shiftModifier:0)|(e.control?ModifierKeys::ctrlModifier:0)|(e.alt?ModifierKeys::altModifier:0)|(e.command?ModifierKeys::commandModifier:0)|(state.modifiers.flags&112)};int code=e.key;if(code>='A'&&code<='Z')code+='a'-'A';if(code>0)state.down.insert(code);KeyPress key{e.key,state.modifiers,e.text?e.text:e.key>=32&&e.key<127?char32_t(e.key):0};if(c.root)c.root->keyStateChanged(true);auto*focus=c.focused.getComponent();if(!focus)focus=c.root.getComponent();for(auto*p=focus;p;p=p->parent_){Component::SafePointer<Component>safe(p);const auto listeners=p->keyListeners_;for(auto*l:listeners){if(!safe)return true;if(std::find(p->keyListeners_.begin(),p->keyListeners_.end(),l)!=p->keyListeners_.end()&&l->keyPressed(key,p))return true;}if(p->keyPressed(key))return true;if(!safe)return true;}
    if(e.key==KeyPress::tabKey&&c.root){std::vector<Component::SafePointer<Component>>order;std::function<void(Component*)>visit=[&](Component*n){if(!n->isShowing()||!n->isEnabled())return;if(n->getWantsKeyboardFocus())order.emplace_back(n);for(auto*child:n->getChildren())visit(child);};visit(c.root);if(!order.empty()){auto at=std::find_if(order.begin(),order.end(),[&](const auto&v){return v.getComponent()==c.focused.getComponent();});int next=at==order.end()?(e.shift?int(order.size())-1:0):(int(at-order.begin())+(e.shift?-1:1)+int(order.size()))%int(order.size());if(order[std::size_t(next)])order[std::size_t(next)]->grabKeyboardFocus();}return true;}return false;}
bool WidgetHost::keyReleased(const cell::ui::KeyEvent&e){::cell::ControlScope control;int code=e.key;if(code>='A'&&code<='Z')code+='a'-'A';auto&state=detail::inputState();state.down.erase(code);state.modifiers=ModifierKeys{(e.shift?ModifierKeys::shiftModifier:0)|(e.control?ModifierKeys::ctrlModifier:0)|(e.alt?ModifierKeys::altModifier:0)|(e.command?ModifierKeys::commandModifier:0)|(state.modifiers.flags&112)};auto lifetime=impl_;auto&c=lifetime->context;if(c.root)return c.root->keyStateChanged(false);return false;}
void WidgetHost::cancelInput(){::cell::ControlScope control;auto lifetime=impl_;auto&c=lifetime->context;if(c.root)c.release(c.root,true);detail::inputState().down.clear();detail::inputState().modifiers={};if(c.root)c.root->keyStateChanged(false);Desktop::getInstance().notifyFocus(nullptr);c.invalidate();}
bool WidgetHost::tick(){::cell::ControlScope control;auto lifetime=impl_;Dispatcher::dispatch();Timer::dispatchPending();return lifetime->context.dirty;}
int WidgetHost::pointerCursor()const noexcept{auto&c=impl_->context;auto*target=c.captured?c.captured.getComponent():c.hovered.getComponent();return target?int(target->getMouseCursor().type):0;}
void WidgetHost::visibility(bool visible){::cell::ControlScope control;auto lifetime=impl_;auto&c=lifetime->context;c.visible=visible;if(!visible)cancelInput();if(c.root)c.root->setVisible(visible);if(c.host)Editor::visibility(visible);}
Point<double>WidgetHost::mousePosition()const noexcept{return {impl_->context.mouse.x,impl_->context.mouse.y};}
void WidgetHost::setScreenOrigin(Point<int>p){impl_->context.screen=p;}
void WidgetHost::invalidateArea(Rectangle<int>){impl_->context.invalidate();}
void WidgetHost::showOverlay(std::unique_ptr<Component>panel){auto lifetime=impl_;auto&c=lifetime->context;dismissOverlay();if(!panel||!c.root)return;c.beforeOverlay=c.focused;c.overlay=std::move(panel);c.root->addAndMakeVisible(*c.overlay);c.overlay->toFront(true);c.invalidate();}
void WidgetHost::dismissOverlay(){auto lifetime=impl_;auto&c=lifetime->context;if(!c.overlay)return;auto closing=std::shared_ptr<Component>(std::move(c.overlay));if(auto*p=closing->getParentComponent())p->removeChildComponent(closing.get());if(c.beforeOverlay)c.beforeOverlay->grabKeyboardFocus();c.beforeOverlay=nullptr;Timer::callAfterDelay(0,[closing]{});c.invalidate();}
}
