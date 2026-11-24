// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <utility>

namespace cell::web::cocoa {
inline bool retainPluginModule() noexcept {
    // A host can release its bundle handle while WebKit still owns a detached
    // Objective-C delegate. Hold a reference to the already loaded image.
    static const auto retained = []() -> void* {
        Dl_info image{};
        if (!dladdr(reinterpret_cast<const void*>(&retainPluginModule), &image) || !image.dli_fname) return nullptr;
        return dlopen(image.dli_fname, RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
    }();
    return retained != nullptr;
}

inline CFRunLoopTimerRef startTimer(void* owner, CFRunLoopTimerCallBack callback) noexcept {
    constexpr auto period = 1. / 30.;
    CFRunLoopTimerContext context{0, owner, nullptr, nullptr, nullptr};
    const auto timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + period,
                                          period, 0, 0, callback, &context);
    if (timer) CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    return timer;
}

inline void stopTimer(CFRunLoopTimerRef& timer) noexcept {
    if (const auto running = std::exchange(timer, nullptr)) {
        CFRunLoopTimerInvalidate(running);
        CFRelease(running);
    }
}
}
