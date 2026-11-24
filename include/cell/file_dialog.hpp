// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/jobs.hpp>
#include <filesystem>
#include <optional>
#include <vector>

namespace cell::ui {
class FileDialog {
public:
    struct Request {
        std::uintptr_t parent{};
        std::string title;
        std::filesystem::path initial;
        std::vector<std::string> extensions;
        bool save{};
        bool directory{};
    };
    struct Response {std::filesystem::path path;bool cancelled{};std::string error;};
    FileDialog();
    ~FileDialog();
    FileDialog(const FileDialog&)=delete;
    FileDialog& operator=(const FileDialog&)=delete;
    static bool available();
    bool open(Request);
    void cancel();
    bool pending()const;
    std::optional<Response> poll();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
