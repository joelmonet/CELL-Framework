#pragma once
#include <cell/ui.hpp>
#include <cell/midi_devices.hpp>

namespace cell::ui {
// A child floating window in the editor's logical coordinate system. It owns
// only controller gestures; instrument routing and sample ownership stay with
// the product. Hooks are serialized on the GUI/control thread.
class InstrumentController {
public:
    enum class Mode {keyboard,pads};
    struct PadInfo {unsigned note{};std::string name,sample;unsigned layers{};bool operator==(const PadInfo&)const=default;};
    struct Hooks {
        std::function<bool(MidiEvent)> send;
        std::function<void()> releaseAll;
        std::function<void()> closed;
        std::function<bool(bool pads)> modeChanged;
    };
    struct DeviceHooks {
        std::function<std::optional<midi::PadInputView>()> poll;
        std::function<bool(std::uint64_t owner,std::uint64_t token)> learn,cancel;
        std::function<bool(std::uint64_t owner,const midi::PadMapping&)> apply;
        std::filesystem::path profilesFile; // empty uses the OS user config dir
    };
    InstrumentController();
    ~InstrumentController();
    InstrumentController(const InstrumentController&)=delete;
    InstrumentController& operator=(const InstrumentController&)=delete;
    void open(std::string title,Hooks,Rect parent,Mode=Mode::keyboard);
    void close();
    bool isOpen()const noexcept;
    bool isMaximized()const noexcept;
    bool isMinimized()const noexcept;
    void restore();
    bool hasKeyboardFocus()const noexcept;
    unsigned padBank()const noexcept;
    bool padLayout(unsigned bank,std::span<const PadInfo>);
    void clearPadLayout();
    void devices(DeviceHooks);
    void libraryFormat(std::string_view extension);
    std::uint64_t focusedControl()const noexcept;
    Rect bounds()const noexcept;
    void paint(Canvas&,Rect parent);
    bool pointer(const PointerEvent&);
    bool keyboard(const KeyEvent&);
    bool keyReleased(const KeyEvent&);
    void cancelInput();
    bool tick();
    std::span<const ControlInfo> controls()const noexcept;
    bool accessibleAction(std::uint64_t,AccessibleAction,double=0);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
