// Original Cell Framework implementation; see LICENSE.
#include <cell/web_meter.hpp>
#include <cell/webview.hpp>
#include <lv2/ui/ui.h>
#include "cell_lv2_web_bindings.hpp"
#include "cell_lv2_web_page.hpp"
#include <cstring>
#include <optional>

namespace {
using cell::web::Event;

class Editor {
public:
    Editor(LV2UI_Controller controller, LV2UI_Write_Function write, const LV2UI_Touch* touch)
        : controller_(controller), write_(write), touch_(touch), values_(fields_.size()),
          bridge_({fields_,
              [this](cell::ParamId id) { return values_[indexOf(id)]; },
              [this](cell::ParamId id) { grab(id, true); return true; },
              [this](cell::ParamId id, double value, bool active) { return edit(id, value, active); },
              [this](cell::ParamId id) { grab(id, false); return true; }},
              cell::designer::parameters) {
        for (std::size_t i = 0; i < fields_.size(); ++i) values_[i] = fields_[i].defaultValue;
    }

    std::uintptr_t open(std::uintptr_t parent, const LV2UI_Resize* hostSize) {
        const auto& descriptor = cell::plugin::descriptor();
#if defined(CELL_LV2_METER_PORT)
        if (firstPort_ + fields_.size() != CELL_LV2_METER_PORT) return 0;
#endif
        const int width = static_cast<int>(std::clamp(descriptor.editorWidth, 1u, 8192u));
        const int height = static_cast<int>(std::clamp(descriptor.editorHeight, 1u, 8192u));
        cell::web::Options options;
        options.parent = parent;
        options.bounds = {0, 0, width, height};
        const std::string_view page(reinterpret_cast<const char*>(cell::assets::lv2EditorPage),
                                    sizeof(cell::assets::lv2EditorPage));
        if (!browser_.open(options) || !browser_.loadHtml(page)) return 0;
        const auto child = browser_.x11Window();
        if (child && hostSize && hostSize->ui_resize) hostSize->ui_resize(hostSize->handle, width, height);
        return child;
    }

    void portEvent(std::uint32_t port, float value) noexcept {
        if (!std::isfinite(value) || port < firstPort_) return;
        const auto index = port - firstPort_;
        if (index < fields_.size()) {
            values_[index] = fields_[index].clamp(value);
            return;
        }
#if defined(CELL_LV2_METER_PORT)
        if (port == CELL_LV2_METER_PORT) levels_.peak = std::clamp(value, 0.0f, 1.0f);
        if (port == CELL_LV2_METER_PORT + 1) levels_.rms = std::clamp(value, 0.0f, 1.0f);
#endif
    }

    int idle() {
        for (const auto& event : browser_.poll()) {
            if (closed_) return 1;
            if (event.kind == Event::Kind::loaded) {
                loaded_ = true;
                lastParameters_.clear();
#if defined(CELL_LV2_METER_PORT)
                lastMeters_.clear();
#endif
            } else if (event.kind == Event::Kind::message && loaded_) {
                bridge_.apply(event.text);
                lastParameters_.clear();
            } else if (event.kind == Event::Kind::error) {
                bridge_.cancel();
                if (!loaded_) return 1;
            }
        }
        if (closed_) return 1;
        if (!loaded_) return 0;
        send(bridge_.snapshot(), lastParameters_);
#if defined(CELL_LV2_METER_PORT)
        send(cell::web::OutputMeter::snapshot(levels_), lastMeters_);
#endif
        return 0;
    }

    void resize(int width, int height) { browser_.setBounds({0, 0, width, height}); }

    void close() {
        if (closed_) return;
        closed_ = true;
        if (singleEdit_) grab(*singleEdit_, false);
        bridge_.cancel();
        touch_ = nullptr;
        browser_.close();
    }

private:
    std::size_t indexOf(cell::ParamId id) const {
        // ParameterBridge only calls the endpoint for a declared binding.
        return std::find_if(fields_.begin(), fields_.end(),
            [id](const auto& parameter) { return parameter.id == id; }) - fields_.begin();
    }

    void grab(cell::ParamId id, bool active) {
        if (!touch_ || !touch_->touch || endingGesture_ == id) return;
        const auto previous = endingGesture_;
        if (!active) endingGesture_ = id;
        touch_->touch(touch_->handle, firstPort_ + indexOf(id), active);
        endingGesture_ = previous;
    }

    bool edit(cell::ParamId id, double value, bool active) {
        const auto index = indexOf(id);
        const auto physical = static_cast<float>(value);
        if (!active) {
            singleEdit_ = id;
            grab(id, true);
        }
        if (closed_) return false;
        values_[index] = value;
        write_(controller_, firstPort_ + index, sizeof(physical), 0, &physical);
        if (!active) grab(id, false);
        singleEdit_.reset();
        return true;
    }

    void send(std::string state, std::string& last) {
        if (state != last && browser_.send(state)) last = std::move(state);
    }

    LV2UI_Controller controller_;
    LV2UI_Write_Function write_;
    const LV2UI_Touch* touch_;
    std::span<const cell::Parameter> fields_{cell::plugin::descriptor().parameters};
    std::uint32_t firstPort_{2u * cell::plugin::descriptor().channels};
    std::vector<double> values_;
    cell::web::View browser_;
    bool closed_{};
    std::optional<cell::ParamId> singleEdit_;
    std::optional<cell::ParamId> endingGesture_;
    cell::web::ParameterBridge bridge_;
    bool loaded_{};
    std::string lastParameters_;
#if defined(CELL_LV2_METER_PORT)
    cell::web::OutputMeter::Levels levels_{};
    std::string lastMeters_;
#endif
};

using Instance = std::shared_ptr<Editor>;

LV2UI_Handle instantiate(const LV2UI_Descriptor*, const char* plugin, const char*, LV2UI_Write_Function write,
                         LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features) noexcept {
    if (widget) *widget = nullptr;
    if (!plugin || std::strcmp(plugin, CELL_LV2_URI) || !write || !widget || !features) return nullptr;
    std::uintptr_t parent{};
    bool idle{};
    const LV2UI_Touch* touch{};
    const LV2UI_Resize* resize{};
    for (auto feature = features; *feature; ++feature) {
        if (!(*feature)->URI) continue;
        const std::string_view uri((*feature)->URI);
        if (uri == LV2_UI__parent) parent = reinterpret_cast<std::uintptr_t>((*feature)->data);
        else if (uri == LV2_UI__idleInterface) idle = true;
        else if (uri == LV2_UI__touch) touch = static_cast<const LV2UI_Touch*>((*feature)->data);
        else if (uri == LV2_UI__resize) resize = static_cast<const LV2UI_Resize*>((*feature)->data);
    }
    if (!parent || !idle) return nullptr;
    try {
        auto instance = std::make_unique<Instance>(std::make_shared<Editor>(controller, write, touch));
        const auto child = (*instance)->open(parent, resize);
        if (!child) return nullptr;
        *widget = reinterpret_cast<LV2UI_Widget>(child);
        return instance.release();
    } catch (...) { return nullptr; }
}

void cleanup(LV2UI_Handle ui) noexcept {
    if (!ui) return;
    auto* instance = static_cast<Instance*>(ui);
    auto editor = std::move(*instance);
    if (!editor) return;
    const std::unique_ptr<Instance> release(instance);
    editor->close();
}

void portEvent(LV2UI_Handle ui, std::uint32_t port, std::uint32_t size, std::uint32_t protocol,
               const void* data) noexcept {
    if (!ui || !data || protocol || size != sizeof(float)) return;
    float value{};
    std::memcpy(&value, data, sizeof(value));
    if (const auto& editor = *static_cast<Instance*>(ui)) editor->portEvent(port, value);
}

const void* extensionData(const char* uri) noexcept {
    static const LV2UI_Idle_Interface idle{[](LV2UI_Handle ui) -> int {
        if (!ui) return 1;
        // Polling GTK and notifying automation can both reenter host code.
        const auto editor = *static_cast<Instance*>(ui);
        try { return editor ? editor->idle() : 1; }
        catch (...) { return 1; }
    }};
    static const LV2UI_Resize resize{nullptr, [](LV2UI_Feature_Handle ui, int width, int height) -> int {
        if (!ui) return 1;
        const auto editor = *static_cast<Instance*>(ui);
        if (!editor) return 1;
        try { editor->resize(width, height); return 0; }
        catch (...) { return 1; }
    }};
    if (uri && !std::strcmp(uri, LV2_UI__idleInterface)) return &idle;
    if (uri && !std::strcmp(uri, LV2_UI__resize)) return &resize;
    return nullptr;
}

const LV2UI_Descriptor descriptor{CELL_LV2_UI_URI, instantiate, cleanup, portEvent, extensionData};
}

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
