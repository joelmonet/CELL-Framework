// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/widgets/controls.hpp>

namespace cell::ui::widgets {
template<class T>struct Range {
    T start{},end{};Range()=default;Range(T a,T b):start(a),end(std::max(a,b)){}
    T getStart()const{return start;}T getEnd()const{return end;}T getLength()const{return end-start;}
    bool isEmpty()const{return end<=start;}bool contains(T v)const{return v>=start&&v<end;}
};
class PopupMenu {
  public:
    class CustomComponent:public Component {bool highlighted_{};public:explicit CustomComponent(bool automatic){setInterceptsMouseClicks(!automatic,!automatic);}virtual void getIdealSize(int&,int&)=0;bool isItemHighlighted()const{return highlighted_;}void setItemHighlighted(bool v){if(v!=highlighted_){highlighted_=v;repaint();}}};
    enum ColourIds{backgroundColourId=0x700,textColourId,highlightedBackgroundColourId,highlightedTextColourId,headerTextColourId};
    struct Item {int id{};Text text;bool enabled=true,ticked{},separator{},heading{};std::shared_ptr<PopupMenu>submenu;std::function<void()>action;std::shared_ptr<CustomComponent>custom;};
    class Options {
      public:
        Component::SafePointer<Component>target,deletionCheck;bool checkDeletion{};std::optional<Rectangle<int>>screen;int minimumWidth=180;
        Options withTargetComponent(Component*c)const{auto o=*this;o.target=c;return o;}
        Options withTargetComponent(Component&c)const{return withTargetComponent(&c);}
        Options withTargetScreenArea(Rectangle<int>r)const{auto o=*this;o.screen=r;return o;}
        Options withMinimumWidth(int w)const{auto o=*this;o.minimumWidth=std::max(80,w);return o;}
        Options withDeletionCheck(Component&c)const{auto o=*this;o.deletionCheck=&c;o.checkDeletion=true;return o;}
    };
    template<class I>requires std::is_integral_v<I>void addItem(I id,Text text,bool enabled=true,bool ticked=false){items_.push_back({int(id),std::move(text),enabled,ticked,false,false,{},{}});}
    void addItem(Text text,std::function<void()>action){items_.push_back({0,std::move(text),true,false,false,false,{},std::move(action)});}
    void addItem(Text text,bool enabled,bool ticked,std::function<void()>action){items_.push_back({0,std::move(text),enabled,ticked,false,false,{},std::move(action)});}
    void addCustomItem(int id,std::unique_ptr<CustomComponent>custom,Text accessible={}){Item item;item.id=id;item.text=std::move(accessible);item.custom=std::move(custom);items_.push_back(std::move(item));}
    void addSeparator(){items_.push_back({0,{},false,false,true,false,{},{}});}
    void addSectionHeader(Text text){items_.push_back({0,std::move(text),false,false,false,true,{},{}});}
    void addSubMenu(Text text,PopupMenu menu,bool enabled=true){items_.push_back({0,std::move(text),enabled,false,false,false,std::make_shared<PopupMenu>(std::move(menu)),{}});}
    int getNumItems()const{return int(items_.size());}const std::vector<Item>&items()const{return items_;}
    void showMenuAsync(Options,std::function<void(int)>callback={})const;
  private:std::vector<Item>items_;
};
class ComboBox:public Component {
    struct Entry {int id;Text text;bool enabled=true,heading{},separator{};};std::vector<Entry>entries_;
    int selected_{};Label label_;bool editable_{};
  public:
    enum ColourIds {backgroundColourId=0x500,textColourId,outlineColourId,buttonColourId,arrowColourId,focusedOutlineColourId};
    ComboBox();explicit ComboBox(Text name):ComboBox(){setName(std::move(name));}
    std::function<void()>onChange;
    void addItem(Text,int);void addItemList(std::initializer_list<Text>list,int first){for(auto&item:list)addItem(item,first++);}
    template<class List>void addItemList(const List&list,int firstID){for(const auto&text:list)addItem(Text{text},firstID++);}
    void addSeparator();void addSectionHeading(Text);void clear(NotificationType=dontSendNotification);
    void setSelectedId(int,NotificationType=sendNotification);int getSelectedId()const{return selected_;}
    void setSelectedItemIndex(int,NotificationType=sendNotification);int getSelectedItemIndex()const;int indexOfItemId(int id)const;void changeItemText(int,Text);
    int getNumItems()const;Text getItemText(int)const;int getItemId(int)const;
    void setItemEnabled(int,bool);void setText(Text,NotificationType=sendNotification);Text getText()const{return label_.getText();}
    void setEditableText(bool);void setJustificationType(Justification j){label_.setJustificationType(j);}
    void setTextWhenNothingSelected(Text);void setTextWhenNoChoicesAvailable(Text);
    void showPopup();void paint(Graphics&)override;void resized()override;
    void mouseDown(const MouseEvent&)override;bool keyPressed(const KeyPress&)override;
  private:Text nothing_,empty_="No choices available";
};
class ScrollBar:public Component {
    double minimum_{},maximum_=1,start_{},size_=1,step_=16,downStart_{};bool autoHide_=true,vertical_=true,dragging_{};
  public:
    enum ColourIds{backgroundColourId=0x600,thumbColourId,trackColourId};
    struct Listener {virtual~Listener()=default;virtual void scrollBarMoved(ScrollBar*,double)=0;};
    explicit ScrollBar(bool vertical=true):vertical_(vertical){}
    void setAutoHide(bool v){autoHide_=v;setVisible(!autoHide_||size_<maximum_-minimum_);}
    void addListener(Listener*);void removeListener(Listener*);
    void setRangeLimits(double start,double end,NotificationType=dontSendNotification);void setRangeLimits(Range<double>r,NotificationType n=dontSendNotification){setRangeLimits(r.start,r.end,n);}
    void setCurrentRange(double start,double size,NotificationType=sendNotification);void setCurrentRange(Range<double>r,NotificationType n=sendNotification){setCurrentRange(r.start,r.getLength(),n);}
    void setCurrentRangeStart(double,NotificationType=sendNotification);double getCurrentRangeStart()const{return start_;}double getCurrentRangeSize()const{return size_;}
    Range<double>getCurrentRange()const{return {start_,start_+size_};}void setSingleStepSize(double n){step_=n;}
    void paint(Graphics&)override;void mouseDown(const MouseEvent&)override;void mouseDrag(const MouseEvent&)override;void mouseUp(const MouseEvent&)override{dragging_=false;}void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&)override;
  private:std::vector<Listener*>listeners_;
};
class Viewport:public Component,private ScrollBar::Listener {
    Component*content_{};std::unique_ptr<Component>owned_;ScrollBar horizontal_{false},vertical_{true};
    int scrollX_{},scrollY_{},thickness_=12;bool showVertical_=true,showHorizontal_=true,updating_{};
    void scrollBarMoved(ScrollBar*,double)override;void updateContent();
  public:
    Viewport();~Viewport()override;
    void setViewedComponent(Component*,bool deleteWhenNoLongerNeeded=true);Component*getViewedComponent()const{return content_;}
    void setViewPosition(int,int);void setViewPosition(Point<int>p){setViewPosition(p.x,p.y);}
    Point<int>getViewPosition()const{return {scrollX_,scrollY_};}int getViewPositionX()const{return scrollX_;}int getViewPositionY()const{return scrollY_;}
    int getMaximumVisibleWidth()const;int getMaximumVisibleHeight()const;
    Rectangle<int>getViewArea()const{return {scrollX_,scrollY_,getMaximumVisibleWidth(),getMaximumVisibleHeight()};}
    ScrollBar&getVerticalScrollBar(){return vertical_;}ScrollBar&getHorizontalScrollBar(){return horizontal_;}
    void setScrollBarsShown(bool vertical,bool horizontal,bool=false,bool=false){showVertical_=vertical;showHorizontal_=horizontal;resized();}
    void setScrollBarThickness(int n){thickness_=std::max(1,n);resized();}void setSingleStepSizes(int horizontal,int vertical){horizontal_.setSingleStepSize(horizontal);vertical_.setSingleStepSize(vertical);}
    void setViewPositionProportionately(double,double);
    std::function<void(const Rectangle<int>&)>onVisibleAreaChanged;
    void resized()override;void childBoundsChanged(Component*)override;void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&)override;
    virtual void visibleAreaChanged(const Rectangle<int>&){}
};
class TableListBox;
class TableListBoxModel {
  public:
    virtual~TableListBoxModel()=default;virtual int getNumRows()=0;
    virtual void paintRowBackground(Graphics&,int,int,int,bool)=0;
    virtual void paintCell(Graphics&,int,int,int,int,bool)=0;
    virtual Component*refreshComponentForCell(int,int,bool,Component*existing){return existing;}
    virtual void deleteKeyPressed(int){}
    virtual void selectedRowsChanged(int){}virtual void cellClicked(int,int,const MouseEvent&){}
    virtual void cellDoubleClicked(int,int,const MouseEvent&){}virtual void backgroundClicked(const MouseEvent&){}
    virtual void sortOrderChanged(int,bool){}
};
class TableHeaderComponent:public Component {
  public:
    enum ColumnFlags {visible=1,resizable=2,draggable=4,appearsOnColumnMenu=8,sortable=16,defaultFlags=31,notSortable=15,notResizableOrSortable=visible};
    struct Column {Text name;int id,width,minWidth,maxWidth,flags;};
    void addColumn(Text,int id,int width,int minWidth=24,int maxWidth=std::numeric_limits<int>::max(),int flags=defaultFlags);
    int getNumColumns(bool=true)const{return int(columns_.size());}int getColumnIdOfIndex(int index,bool=true)const{return index>=0&&std::size_t(index)<columns_.size()?columns_[std::size_t(index)].id:0;}
    int getColumnIndexFromId(int,bool=true)const;int getIndexOfColumnId(int id,bool visibleOnly=true)const{return getColumnIndexFromId(id,visibleOnly);}
    Rectangle<int>getColumnPosition(int index)const;void resizeAllColumnsToFit(int width);int getColumnWidth(int)const;void setColumnWidth(int,int);
    int getTotalWidth()const;const std::vector<Column>&columns()const{return columns_;}
    void setSortColumnId(int,bool);void paint(Graphics&)override;void mouseDown(const MouseEvent&)override;
    std::function<void(int,bool)>onSort;std::function<void()>onGeometryChanged;
  private:std::vector<Column>columns_;int sort_{};bool forward_=true;
};
class TableListBox:public Component {
    class Content;std::unique_ptr<Content>content_;Viewport viewport_;TableHeaderComponent header_;
    TableListBoxModel*model_{};int rowHeight_=30,headerHeight_=26,selected_=-1;
    friend class Content;
  public:
    TableListBox();TableListBox(Text name,TableListBoxModel*m):TableListBox(){setName(std::move(name));setModel(m);}
    ~TableListBox()override;
    void setModel(TableListBoxModel*);TableHeaderComponent&getHeader(){return header_;}
    void setRowHeight(int v){rowHeight_=std::max(1,v);updateContent();}int getRowHeight()const{return rowHeight_;}
    void setHeaderHeight(int v){headerHeight_=std::max(0,v);resized();}void updateContent();
    void selectRow(int,bool dontScroll=false,bool=false);int getSelectedRow(int=0)const{return selected_;}
    void deselectAllRows(){selectRow(-1,true);}void scrollToEnsureRowIsOnscreen(int);
    Viewport*getViewport(){return &viewport_;}void resized()override;bool keyPressed(const KeyPress&)override;
};
}
