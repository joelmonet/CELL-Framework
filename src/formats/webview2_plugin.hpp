// Original Cell Framework implementation; see LICENSE.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace cell::web::win32 {
inline std::filesystem::path pluginProfile(const wchar_t* format, const char* directory) {
    PWSTR path{};
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, &CoTaskMemFree);
    if (FAILED(result) || !path) throw std::runtime_error("Cannot locate the Windows local app data folder for WebView2");
    return std::filesystem::path(path) / L"Cell" / L"WebView2" / format / directory;
}

inline HMODULE pluginModule() noexcept {
    // A DAW can release its module handle while WebView2 still owns an STA
    // callback. Keep the module mapped once a browser editor opens.
    static const auto retained = [] {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&pluginModule), &module)) return HMODULE{};
        return module;
    }();
    return retained;
}
}
