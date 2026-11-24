// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/widgets/component.hpp>
#include <cell/ui_values.hpp>

namespace cell::ui::widgets {
class TextEditor;
class Button:public Component {
    Text text_;bool toggle_{},clickToggle_{},armed_{};int radio_{};
  public:
    explicit Button(Text text={}):Component(text),text_(std::move(text)){setWantsKeyboardFocus(true);}
    enum ColourIds {buttonColourId=0x100,buttonOnColourId,textColourOffId,textColourOnId};
    std::function<void()>onClick,onStateChange;
    void setButtonText(Text t){if(t==text_)return;text_=std::move(t);repaint();}const Text&getButtonText()const{return text_;}
    void setClickingTogglesState(bool v){clickToggle_=v;}bool getClickingTogglesState()const{return clickToggle_;}
    void setToggleState(bool,NotificationType);bool getToggleState()const{return toggle_;}
    void setRadioGroupId(int group,NotificationType=dontSendNotification){radio_=group;}int getRadioGroupId()const{return radio_;}
    enum ButtonState{buttonNormal,buttonOver,buttonDown};
    void setState(ButtonState state){armed_=state==buttonDown;repaint();if(onStateChange)onStateChange();}
    bool isDown()const{return armed_;}void triggerClick();
    virtual void clicked(){}virtual void paintButton(Graphics&,bool,bool);
    void paint(Graphics&g)override{paintButton(g,isMouseOver(true),armed_);}
    void mouseDown(const MouseEvent&)override;void mouseUp(const MouseEvent&)override;
    void mouseCaptureLost()override{armed_=false;repaint();}
    bool keyPressed(const KeyPress&)override;
};
class TextButton:public Button {
  public:using Button::Button;
    void paintButton(Graphics&,bool,bool)override;
};
class ToggleButton:public Button {
  public:
    static constexpr int textColourId=Button::textColourOffId;explicit ToggleButton(Text text={}):Button(std::move(text)){setClickingTogglesState(true);}
    void paintButton(Graphics&,bool,bool)override;
};
class TextEditor:public Component {
    Text text_;Font font_{12};bool readOnly_{},multiline_{},wrap_=true;
    int caret_{},selectionStart_{},selectionEnd_{};float yOffset_{};
    bool caretVisible_=true,scrollbars_=true,returnStartsNewLine_=true,selecting_{},scrollDragging_{};
    BorderSize<int>border_{3,5,3,5};
    struct VisualLine{int begin,end;};std::vector<VisualLine>lines_;
    Text cachedText_;float cachedWidth_=-1,cachedSize_{};bool cachedWrap_{};
    void layoutText();Rectangle<float>textArea()const;int characterAt(Point<float>);
    struct Change{int at;Text removed,added;};std::vector<Change>history_;std::size_t historyAt_{};
    void replaceSelection(Text,bool record=true);void notify();
  public:
    enum ColourIds {backgroundColourId=0x200,textColourId,highlightColourId,highlightedTextColourId,outlineColourId,focusedOutlineColourId};
    TextEditor();explicit TextEditor(Text name);
    std::function<void()>onTextChange,onReturnKey,onEscapeKey,onFocusLost;
    void setText(Text,bool send=true);const Text&getText()const{return text_;}
    void setReadOnly(bool v){readOnly_=v;}bool isReadOnly()const{return readOnly_;}
    void setMultiLine(bool multiline,bool wrap=true){multiline_=multiline;wrap_=wrap;repaint();}
    void setReturnKeyStartsNewLine(bool v){returnStartsNewLine_=v;}
    void setCaretVisible(bool v){caretVisible_=v;repaint();}
    void setScrollbarsShown(bool v){scrollbars_=v;cachedWidth_=-1;repaint();}
    void setBorder(BorderSize<int>b){border_=b;cachedWidth_=-1;repaint();}
    void setFont(Font f){font_=f;repaint();}Font getFont()const{return font_;}
    void setCaretPosition(int p){caret_=std::clamp(p,0,text_.length());selectionStart_=selectionEnd_=caret_;repaint();}
    int getCaretPosition()const{return caret_;}void selectAll(){selectionStart_=0;selectionEnd_=text_.length();caret_=selectionEnd_;repaint();}
    Text getHighlightedText()const{return text_.substring(std::min(selectionStart_,selectionEnd_),std::max(selectionStart_,selectionEnd_));}
    void insertTextAtCaret(Text t){replaceSelection(std::move(t));}
    void clear(){setText({},true);}void applyFontToAllText(Font f,bool=true){setFont(f);}
    void setTextToShowWhenEmpty(Text,Colour);
    void paint(Graphics&)override;bool keyPressed(const KeyPress&)override;
    void mouseDown(const MouseEvent&)override;void mouseDrag(const MouseEvent&)override;void mouseUp(const MouseEvent&)override{selecting_=scrollDragging_=false;}void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&)override;
    void mouseCaptureLost()override{selecting_=scrollDragging_=false;}
    void focusLost()override{if(onFocusLost)onFocusLost();repaint();}
  private:Text placeholder_;Colour placeholderColor_{0xff8c9998};
};
class Label:public Component {
    Text text_;Font font_{12};Justification justification_{Justification::centredLeft};
    bool singleClick_{},doubleClick_{};float minimumScale_=1.f;BorderSize<int>border_{1,5,1,5};std::unique_ptr<TextEditor>editor_;
  public:
    enum ColourIds {backgroundColourId=0x300,textColourId,outlineColourId,textWhenEditingColourId,backgroundWhenEditingColourId,outlineWhenEditingColourId};
    Label()=default;Label(Text name,Text text):Component(std::move(name)),text_(std::move(text)){}
    std::function<void()>onTextChange,onEditorShow,onEditorHide;
    void setText(Text,NotificationType);const Text&getText(bool=false)const{return text_;}
    void setFont(Font f){font_=f;repaint();}Font getFont()const{return font_;}
    void setJustificationType(Justification j){justification_=j;repaint();}Justification getJustificationType()const{return justification_;}
    void setEditable(bool single,bool twice=false,bool=false){singleClick_=single;doubleClick_=twice;}
    bool isBeingEdited()const{return bool(editor_);}TextEditor*getCurrentTextEditor()const{return editor_.get();}
    void showEditor();void hideEditor(bool discard);
    void setMinimumHorizontalScale(float v){minimumScale_=std::clamp(v,.01f,1.f);repaint();}float getMinimumHorizontalScale()const{return minimumScale_;}
    BorderSize<int>getBorderSize()const{return border_;}void setBorderSize(BorderSize<int>b){border_=b;repaint();}
    void paint(Graphics&)override;void resized()override;
    void mouseDown(const MouseEvent&e)override{if(singleClick_&&e.mods.isLeftButtonDown())showEditor();}
    void mouseDoubleClick(const MouseEvent&)override{if(doubleClick_)showEditor();}
};
class Slider:public Component {
    double minimum_{},maximum_=1.,interval_{},value_{},skew_=1.,startValue_{},defaultValue_{};
    widgets::SliderStyle style_=widgets::SliderStyle::linearHorizontal;int decimals_=2;bool dragging_{},snapsToMouse_=true,doubleDefault_{},popupDragging_{},popupHover_{},scrollWheel_=true;
    Text suffix_;Label valueLabel_;TextButton decrement_{"−"},increment_{"+"};int textWidth_=64,textHeight_=24;
  public:
    void updateText();
  private:
  public:
    class ScopedDragNotification {SafePointer<Slider>slider_;public:explicit ScopedDragNotification(Slider&);~ScopedDragNotification();};
    using SliderStyle=widgets::SliderStyle;
    static constexpr auto LinearHorizontal=SliderStyle::linearHorizontal,LinearVertical=SliderStyle::linearVertical,RotaryHorizontalVerticalDrag=SliderStyle::rotary,IncDecButtons=SliderStyle::incDecButtons;
    enum TextEntryBoxPosition {NoTextBox,TextBoxLeft,TextBoxRight,TextBoxAbove,TextBoxBelow};
    enum ColourIds {backgroundColourId=0x400,thumbColourId,trackColourId,textBoxTextColourId,textBoxBackgroundColourId,textBoxHighlightColourId,textBoxOutlineColourId};
    Slider();explicit Slider(SliderStyle,TextEntryBoxPosition=NoTextBox);
    std::function<void()>onValueChange,onDragStart,onDragEnd;
    std::function<Text(double)>textFromValueFunction;
    void setRange(double minimum,double maximum,double interval=0);double getMinimum()const{return minimum_;}double getMaximum()const{return maximum_;}double getInterval()const{return interval_;}
    void setValue(double,NotificationType=sendNotification);double getValue()const{return value_;}
    void setSliderStyle(SliderStyle style){style_=style;decrement_.setVisible(style==IncDecButtons);increment_.setVisible(style==IncDecButtons);resized();repaint();}SliderStyle getSliderStyle()const{return style_;}
    void setTextBoxStyle(TextEntryBoxPosition,bool readOnly,int width,int height);
    void setTextValueSuffix(Text s){suffix_=std::move(s);updateText();}void setNumDecimalPlacesToDisplay(int n){decimals_=n;updateText();}
    void setSkewFactor(double v,bool=false){if(std::isfinite(v)&&v>0)skew_=v;}void setSkewFactorFromMidPoint(double);
    void setDoubleClickReturnValue(bool enabled,double value,ModifierKeys={}){doubleDefault_=enabled;defaultValue_=value;}
    void setPopupDisplayEnabled(bool,bool,Component* =nullptr,int=0);
    void setSliderSnapsToMousePosition(bool v){snapsToMouse_=v;}
    void setScrollWheelEnabled(bool v){scrollWheel_=v;}
    virtual Text getTextFromValue(double value);virtual double getValueFromText(const Text&);
    virtual double valueToProportionOfLength(double);virtual double proportionOfLengthToValue(double);
    bool isMouseButtonDown()const{return dragging_;}
    void paint(Graphics&)override;void resized()override;
    void mouseDown(const MouseEvent&)override;void mouseDrag(const MouseEvent&)override;void mouseUp(const MouseEvent&)override;
    void mouseDoubleClick(const MouseEvent&)override;void mouseWheelMove(const MouseEvent&,const MouseWheelDetails&)override;
    void mouseCaptureLost()override;bool keyPressed(const KeyPress&)override;
  private:TextEntryBoxPosition textPosition_=NoTextBox;
};
class Clipboard {
  public:
    static Text getText();static void setText(Text);
    // Transitional platform services may be injected by a format adapter.
    // The widget renderer/input implementation has no host GUI dependency.
    static void setBackend(std::function<Text()>reader,std::function<void(const Text&)>writer);
};
struct SystemClipboard{static Text getTextFromClipboard(){return Clipboard::getText();}static void copyTextToClipboard(Text text){Clipboard::setText(std::move(text));}};

}
