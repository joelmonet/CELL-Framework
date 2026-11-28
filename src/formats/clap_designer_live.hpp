// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <cell/detail/web_document.hpp>
#include <cell/files.hpp>
#include <cell/web_parameter_bridge.hpp>
#include <chrono>
#include <cstdlib>

namespace cell::web {

class DesignerLivePage {
public:
    using Clock = std::chrono::steady_clock;
    struct Update {
        std::string html;
        std::string appearanceScript;
    };

    explicit DesignerLivePage(std::string_view pluginId) {
        const auto* id = std::getenv("CELL_DESIGNER_LIVE_ID");
        if (!id || pluginId != id) return;
#if defined(_WIN32)
        const auto* directory = _wgetenv(L"CELL_DESIGNER_LIVE_DIR");
#else
        const auto* directory = std::getenv("CELL_DESIGNER_LIVE_DIR");
#endif
        if (directory && *directory) {
            const std::filesystem::path root(directory);
            if (root.is_absolute()) file_ = root / "cell-designer-live.html";
        }
    }

    std::optional<Update> read(const ParameterBridge& bridge) {
        const auto now = Clock::now();
        if (file_.empty() || pending() || now < nextRead_) return {};
        nextRead_ = now + std::chrono::milliseconds(250);
        std::error_code error;
        const auto time = std::filesystem::last_write_time(file_, error);
        if (error) return {};
        const auto size = std::filesystem::file_size(file_, error);
        if (error || stamp_ == Stamp{time, size}) return {};
        if (size > detail::maxHtmlBytes) {
            stamp_ = Stamp{time, size};
            throw std::runtime_error("Designer live page exceeds 8 MiB");
        }
        const auto bytes = files::readBounded(file_, detail::maxHtmlBytes);
        const auto after = std::filesystem::last_write_time(file_, error);
        if (error || time != after || size != bytes.size()) return {};
        stamp_ = Stamp{time, size};
        if (bytes.empty()) throw std::runtime_error("Empty Designer live page");
        const std::string_view content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        constexpr std::string_view prefix = "<!-- cell.designer.live.v1 ";
        const auto newline = content.find('\n');
        if (!content.starts_with(prefix) || newline == content.npos || newline > detail::maxMessageBytes ||
            !content.substr(0, newline).ends_with("-->"))
            throw std::runtime_error("Publish a live page from Cell UI Designer");
        const auto contract = json::read(content.substr(prefix.size(), newline - prefix.size() - 3),
                                        {detail::maxMessageBytes, 2048, detail::maxMessageBytes, 8});
        validate(contract, bridge);
        std::string html(content.substr(newline + 1));
        if (!html.starts_with("<!doctype html>") || html.find("<head>") == html.npos ||
            html.find('\0') != html.npos || html.size() > detail::maxHtmlBytes - 128)
            throw std::runtime_error("Invalid Designer live document");
        constexpr std::string_view appearanceLine = "\nconst cellLiveAppearance = ";
        std::string shell, script;
        if (const auto marker = html.find(appearanceLine); marker != html.npos) {
            const auto start = marker + appearanceLine.size();
            const auto end = html.find('\n', start);
            if (end == html.npos || end <= start || html[end - 1] != ';' ||
                html.find(appearanceLine, end) != html.npos)
                throw std::runtime_error("Invalid Designer live appearance line");
            const std::string_view payload = std::string_view(html).substr(start, end - start - 1);
            const auto appearance = json::read(payload);
            if (!appearance["css"].string() || !appearance["fonts"].string() ||
                !appearance["looks"].array() || appearance["looks"].array()->size() > 256)
                throw std::runtime_error("Invalid Designer live appearance data");
            shell = html;
            shell.erase(start, end - start);
            if (active() && shell == shell_ && payload.size() < 1024u * 1024) {
                // Quote the original JSON as data to preserve its text and
                // prevent it from introducing JavaScript into the update.
                auto quoted = json::write(json::Value(std::string(payload)));
                script = "window.__cellApplyDesignerAppearance(JSON.parse(" + quoted + "));";
            }
        }
        shell_ = std::move(shell);
        return Update{std::move(html), std::move(script)};
    }

    void mount(std::string& html) {
        const auto head = html.find("<head>");
        const auto token = std::to_string(++generation_);
        html.insert(head + 6, "<script>window.__cellDesignerLivePage=" + token + ";</script>");
        token_ = token;
        deadline_ = Clock::now() + std::chrono::seconds(6);
    }

    bool pending() const noexcept { return deadline_ != Clock::time_point{}; }
    bool active() const noexcept { return !token_.empty(); }
    bool timedOut() const noexcept { return pending() && Clock::now() >= deadline_; }

    bool accepts(const json::Value& message) const {
        return active() && message["designerPage"].text() == token_;
    }
    void ready() noexcept { deadline_ = {}; }
    void reset() noexcept {
        fallback();
        stamp_.reset();
    }
    void fallback() noexcept {
        token_.clear();
        shell_.clear();
        deadline_ = {};
    }
private:
    struct Stamp {
        std::filesystem::file_time_type time;
        std::uintmax_t size;
        bool operator==(const Stamp&) const = default;
    };

    static void validate(const json::Value& contract, const ParameterBridge& bridge) {
        const auto* parameters = contract["parameters"].array();
        const auto* actions = contract["actions"].array();
        if (!parameters || parameters->size() > 128 || !actions || actions->size() > 128)
            throw std::runtime_error("Invalid Designer live bindings");
        for (const auto& item : *parameters) {
            const auto* writable = item["writable"].boolean();
            if (!writable || !bridge.allowsParameter(item["key"].text(), *writable))
                throw std::runtime_error("Designer changed a compiled parameter binding. Re-export bindings and rebuild.");
        }
        for (const auto& item : *actions) {
            const auto mode = item["mode"].text();
            if ((mode != "invoke" && mode != "momentary") ||
                !bridge.allowsAction(item["key"].text(), mode == "invoke" ? ActionMode::invoke : ActionMode::momentary))
                throw std::runtime_error("Designer changed a compiled action binding. Re-export bindings and rebuild.");
        }
    }

    std::filesystem::path file_;
    std::optional<Stamp> stamp_;
    std::uint64_t generation_{};
    std::string token_, shell_;
    Clock::time_point nextRead_{}, deadline_{};
};

} // namespace cell::web
