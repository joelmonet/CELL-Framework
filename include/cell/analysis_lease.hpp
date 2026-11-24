// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
namespace cell {
// A control thread leases visual analysis for a number of audio frames. One
// audio consumer advances the clock; no wall clock, CAS loop, allocation or
// lock is needed by consume(). A current block can finish after stop/expiry.
class AnalysisLease {
 static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"AnalysisLease needs lock-free 64-bit atomics");
 std::uint64_t cursor_{}; // audio consumer only
 std::atomic<std::uint64_t> published_{0},deadline_{0};
 static bool future(std::uint64_t end,std::uint64_t now) noexcept {
  const auto distance=end-now;return distance&&distance<(std::uint64_t{1}<<63);
 }
public:
 void renew(std::uint32_t frames) noexcept {
  deadline_.store(published_.load(std::memory_order_acquire)+frames,std::memory_order_release);
 }
 void stop() noexcept {renew(0);}
 // One callback owner, including when inactive. Frames must describe the next
 // real block; zero does not advance the lease or admit analysis.
 bool consume(std::uint32_t frames) noexcept {
  if(!frames)return false;
  const auto begin=cursor_;cursor_+=frames;
  published_.store(cursor_,std::memory_order_release);
  return future(deadline_.load(std::memory_order_acquire),begin);
 }
 bool active() const noexcept {
  return future(deadline_.load(std::memory_order_acquire),published_.load(std::memory_order_acquire));
 }
};
}
