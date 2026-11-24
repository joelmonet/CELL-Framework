// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <chrono>
#include <exception>
#include <functional>
#include <cstddef>
#include <mutex>
namespace cell {
// Serializes native control/model operations, never an audio callback. A view
// can use try_to_lock to skip a frame during a host model transaction.
class ControlScope {
 std::unique_lock<std::recursive_mutex> lock_;
 public:ControlScope();explicit ControlScope(std::try_to_lock_t);bool ownsLock()const noexcept{return lock_.owns_lock();}
};
// Non-realtime message delivery. Posting is thread-safe; the owning platform
// loop dispatches on its main thread, including when editor windows are hidden.
// Admission follows memory, with no document/track/job count ceiling.
class MainThreadDispatcher {
 public:
 static bool post(std::function<void()>);
 // Data/model completions contain no GUI or third-party GUI lifecycle work.
 // A headless ARA model owner may service these under ControlScope even after
 // an editor has closed. The ordinary queue stays bound to the GUI thread.
 static bool postControl(std::function<void()>);
 static void dispatchControl(std::chrono::nanoseconds budget=std::chrono::milliseconds(3));
 // Delays are serviced by the same owner loop; no sleeping worker per timer.
 static bool postAfter(std::chrono::milliseconds,std::function<void()>);
 // Bind once on the UI/control owner. Before binding, headless control calls
 // are allowed on the host's serialized model thread.
 static void bindCurrentThread();
 static bool isOwnerThread();
 // A time budget yields unfinished callbacks to the next pump; none are dropped.
 // Reentrant dispatches yield to the outer dispatch. Without an error handler,
 // the first callback exception is rethrown after the remaining work in this turn.
 static void dispatch(std::chrono::nanoseconds budget=std::chrono::milliseconds(3));
 static std::size_t pending();
 static void setExceptionHandler(std::function<void(std::exception_ptr)>);
 private:static void dispatchImpl(std::chrono::nanoseconds,bool);
};
}
