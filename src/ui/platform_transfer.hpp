// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/ui.hpp>
#if defined(__linux__)
#include <X11/Xlib.h>
#include <atomic>
#endif
namespace cell::ui::native {
void installClipboard();
void pollTransfers();
bool beginFileDrag(std::uintptr_t,std::span<const std::string>,bool);
#if defined(__linux__)
void registerDropTarget(Display*,::Window,Editor&);
void unregisterDropTarget(::Window);
bool handleDropEvent(Display*,XEvent&,Editor&);
#else
void*registerDropTarget(std::uintptr_t,Editor&);
void unregisterDropTarget(void*);
#endif
}
