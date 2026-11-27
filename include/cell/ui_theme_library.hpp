// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cell/file.hpp>
#include <cell/ui_theme.hpp>
#include <mutex>
namespace cell::ui::themes {
struct Snapshot {
    Theme theme;
    int stockIndex = -1;
    std::filesystem::path userFile;
    std::uint64_t revision{};
};
// Host GUI/control threads only. Built-in definitions are copied and immutable;
// user themes and the active selection are separate files beneath directory.
class Library {
public:
    Library(std::filesystem::path directory, std::vector<Theme> stocks)
        : directory_(std::move(directory)), stocks_(std::move(stocks)) {
        if (directory_.empty() || stocks_.empty() || stocks_.size() > 64)
            throw std::invalid_argument("Theme library needs a directory and 1–64 stock themes");
        for (const auto& t : stocks_)
            t.validate();
        current_ = {stocks_[0], 0, {}};
        try {
            const auto file = directory_ / "active.celltheme";
            if (std::filesystem::exists(file)) {
                current_.theme = Theme::load(file);
                current_.stockIndex = -1;
                for (unsigned i = 0; i < stocks_.size(); ++i)
                    if (current_.theme == stocks_[i])
                        current_.stockIndex = int(i);
            }
        } catch (const std::exception&) {
            current_ = {stocks_[0], 0, {}};
        }
    }
    const std::vector<Theme>& stocks() const noexcept {
        return stocks_;
    }
    const Theme& stock(unsigned index) const {
        if (index >= stocks_.size())
            throw std::out_of_range("Stock theme index");
        return stocks_[index];
    }
    const std::filesystem::path& folder() const noexcept {
        return directory_;
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        auto result = current_;
        result.revision = revision_.load(std::memory_order_relaxed);
        return result;
    }
    std::uint64_t revision() const noexcept {
        return revision_.load(std::memory_order_acquire);
    }
    void preview(Snapshot value) {
        value.theme.validate();
        if (value.stockIndex >= 0 &&
            (!(value.theme == stock(unsigned(value.stockIndex))) || !value.userFile.empty()))
            throw std::invalid_argument("Stock themes are immutable");
        std::lock_guard lock(mutex_);
        setCurrent(std::move(value));
    }
    void select(unsigned index) {
        preview({stock(index), int(index), {}});
    }
    void edit(Theme theme) {
        theme.validate();
        std::lock_guard lock(mutex_);
        auto prior = current_;
        if (prior.stockIndex >= 0) {
            prior.userFile.clear();
            theme.name = userName(std::move(theme.name), " Custom");
        }
        prior.stockIndex = -1;
        prior.theme = std::move(theme);
        setCurrent(std::move(prior));
    }
    void apply() {
        std::lock_guard lock(mutex_);
        writeActive(current_.theme);
    }
    std::filesystem::path saveNamed(std::string name) {
        std::lock_guard lock(mutex_);
        auto value = current_;
        value.theme.name = userName(std::move(name), " Custom");
        value.theme.validate();
        const auto filename = File::createLegalFileName(Text(value.theme.name)) + ".celltheme";
        std::filesystem::create_directories(directory_ / "User");
        const auto path = File(directory_ / "User").getChildFile(filename).nativePath();
        value.theme.exportFile(path);
        value.stockIndex = -1;
        value.userFile = path;
        writeActive(value.theme);
        setCurrent(std::move(value));
        return path;
    }
    void importFile(const std::filesystem::path& path) {
        auto theme = Theme::load(path);
        theme.name = userName(std::move(theme.name), " Imported");
        preview({std::move(theme), -1, {}});
    }
    void loadUser(const std::filesystem::path& path) {
        const auto file = std::filesystem::canonical(path);
        if (file.extension() != ".celltheme" ||
            !std::filesystem::equivalent(file.parent_path(), directory_ / "User"))
            throw std::invalid_argument("Choose a saved theme from the User folder or use Import");
        auto theme = Theme::load(file);
        theme.name = userName(std::move(theme.name), " Imported");
        preview({std::move(theme), -1, file});
    }
    void exportFile(const std::filesystem::path& path) const {
        snapshot().theme.exportFile(path);
    }
    std::vector<std::filesystem::path> userThemes() const {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        const auto path = directory_ / "User";
        for (std::filesystem::directory_iterator it(path, error), end; it != end && !error;
             it.increment(error))
            if (it->is_regular_file(error) && it->path().extension() == ".celltheme")
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        return files;
    }

private:
    void setCurrent(Snapshot value) {
        current_ = std::move(value);
        revision_.fetch_add(1, std::memory_order_release);
    }
    void writeActive(const Theme& theme) const {
        std::filesystem::create_directories(directory_);
        theme.exportFile(directory_ / "active.celltheme");
    }
    std::string userName(std::string name, std::string_view suffix) const {
        for (const auto& stock : stocks_) {
            if (name != stock.name)
                continue;
            auto length = std::min(name.size(), Theme::maxNameBytes - suffix.size());
            while (length < name.size() &&
                   (static_cast<unsigned char>(name[length]) & 0xc0) == 0x80)
                --length;
            name.resize(length);
            name.append(suffix);
            break;
        }
        return name;
    }

    std::filesystem::path directory_;
    std::vector<Theme> stocks_;
    mutable std::mutex mutex_;
    Snapshot current_;
    std::atomic<std::uint64_t> revision_{1};
};
} // namespace cell::ui::themes
