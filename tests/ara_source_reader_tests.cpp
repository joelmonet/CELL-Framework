#include <chrono>
// Original Cell Framework implementation; see LICENSE.
#include <cell/ara/source_reader.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool watchAllocations = false;
std::atomic<unsigned> allocations{0};
void check(bool v, const char *message) { if (!v) throw std::runtime_error(message); }
float sample(unsigned c, int64_t at) {
    return float(int((at % 4093 * 17 + c * 3) % 4093) - 2046) / 4096.f;
}
struct Host {
    unsigned channels = 1;
    std::atomic<int> creates{0}, destroys{0}, reads{0}, largest{0}, active{0}, badDestroy{0};
    std::atomic<bool> failCreate{false}, failRead{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, entered = false, release = false;
    static Host &get(ARA::ARAAudioAccessControllerHostRef ref) {
        return *reinterpret_cast<Host *>(ref);
    }
    static ARA::ARAAudioReaderHostRef ARA_CALL create(ARA::ARAAudioAccessControllerHostRef ref,
                                                      ARA::ARAAudioSourceHostRef,
                                                      ARA::ARABool use64) noexcept {
        auto &h = get(ref); ++h.creates;
        if (h.failCreate || use64 != ARA::kARAFalse) return nullptr;
        return reinterpret_cast<ARA::ARAAudioReaderHostRef>(&h);
    }
    static ARA::ARABool ARA_CALL read(ARA::ARAAudioAccessControllerHostRef ref,
                                    ARA::ARAAudioReaderHostRef reader, ARA::ARASamplePosition at,
                                    ARA::ARASampleCount count, void *const output[]) noexcept {
        auto &h = get(ref); ++h.active; ++h.reads;
        auto largest = h.largest.load();
        while (largest < count && !h.largest.compare_exchange_weak(largest, int(count))) {}
        if (reader != reinterpret_cast<ARA::ARAAudioReaderHostRef>(&h) || at < 0 || count <= 0) {
            --h.active; return ARA::kARAFalse;
        }
        {
            std::unique_lock<std::mutex> lock(h.mutex);
            if (h.block) {
                h.entered = true; h.changed.notify_all();
                h.changed.wait(lock, [&] { return h.release; });
            }
        }
        for (unsigned c = 0; c < h.channels; ++c)
            for (int64_t n = 0; n < count; ++n)
                static_cast<float *>(output[c])[n] = sample(c, at + n);
        --h.active;
        return h.failRead ? ARA::kARAFalse : ARA::kARATrue;
    }
    static void ARA_CALL destroy(ARA::ARAAudioAccessControllerHostRef ref,
                                 ARA::ARAAudioReaderHostRef) noexcept {
        auto &h = get(ref); ++h.destroys;
        if (h.active) ++h.badDestroy;
    }
    ARA::ARAAudioAccessControllerInterface api{sizeof(ARA::ARAAudioAccessControllerInterface),
                                               create, read, destroy};
    auto ref() { return reinterpret_cast<ARA::ARAAudioAccessControllerHostRef>(this); }
    auto source() { return reinterpret_cast<ARA::ARAAudioSourceHostRef>(this); }
};
}
void *operator new(std::size_t bytes) {
    if (watchAllocations) ++allocations;
    if (auto *p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
int main() {
    using namespace std::chrono_literals;
    try {
        for (unsigned channels : {1u, 2u, 32u, 64u}) {
            Host host; host.channels = channels;
            cell::ara::SourceReader reader;
            const int64_t origin = int64_t(std::numeric_limits<int32_t>::max()) + 940;
            check(reader.open(host.ref(), &host.api, host.source(), {192000., channels, origin + 30000}),
                  "Native ARA source failed to open");
            check(!reader.open(host.ref(), &host.api, host.source(), {48000., channels, 1}),
                  "Open replaced an active host reader without a lifecycle callback");
            const auto snapshot=reader.snapshot();
            std::vector<std::vector<float>> pcm(channels, std::vector<float>(25000, 1.f));
            std::vector<float *> pointers; for (auto &c : pcm) pointers.push_back(c.data());
            allocations = 0; watchAllocations = true;
            const bool ok = reader.read(pointers, snapshot, origin, 25000);
            watchAllocations = false;
            check(ok && allocations == 0, "Native ARA worker read allocated or failed");
            for (unsigned c = 0; c < channels; ++c)
                for (int n = 0; n < 25000; ++n)
                    check(pcm[c][size_t(n)] == sample(c, origin + n), "Native ARA changed sample values/order");
            check(host.largest <= cell::ara::SourceReader::readQuantum && host.reads == 4,
                  "Long source read was not divided into bounded host calls");
            check(reader.read(pointers, snapshot, -19, 100), "Source beginning padding failed");
            for (unsigned c = 0; c < channels; ++c)
                for (int n = 0; n < 100; ++n)
                    check(pcm[c][size_t(n)] == (n < 19 ? 0.f : sample(c, n - 19)), "Beginning padding changed PCM");
            check(reader.read(pointers, snapshot, origin + 29977, 100), "Source tail padding failed");
            for (unsigned c = 0; c < channels; ++c)
                for (int n = 0; n < 100; ++n)
                    check(pcm[c][size_t(n)] == (n < 23 ? sample(c, origin + 29977 + n) : 0.f),
                          "Tail padding changed PCM");
            for (auto at : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
                check(reader.read(pointers, snapshot, at, 100), "Extreme source boundary overflowed");
                for (auto &c : pcm) check(std::all_of(c.begin(), c.begin() + 100, [](float x) { return x == 0.f; }),
                                          "Out-of-source data was not silent");
            }
            host.failRead = true;
            check(!reader.read(pointers, snapshot, 0, 100), "Failed host read was accepted");
            for (auto &c : pcm) check(std::all_of(c.begin(), c.begin() + 100, [](float x) { return x == 0.f; }),
                                      "Failed host read leaked partial output");
            reader.close(); reader.close();
            check(host.destroys == 1 && !reader.read(pointers, snapshot, 0, 100), "Closed ARA reader remained callable");
            host.failCreate = true;
            check(!reader.open(host.ref(), &host.api, host.source(), {48000., channels, 100}),
                  "Host reader creation failure was accepted");
            check(host.destroys == 1 && host.badDestroy == 0, "Null/active host reader was destroyed");
        }
        {
            Host first;cell::ara::SourceReader reader;
            check(reader.open(first.ref(),&first.api,first.source(),{48000.,1,100}),"Initial source open failed");
            const auto old=reader.snapshot();reader.close();
            Host replacement;replacement.channels=64;
            check(reader.open(replacement.ref(),&replacement.api,replacement.source(),{96000.,64,1000}),"Replacement source open failed");
            const auto current=reader.snapshot();
            std::array<float,127> oldAudio;oldAudio.fill(.75f);float* oldChannels[]{oldAudio.data()};
            check(!reader.read(oldChannels,old,0,127)&&replacement.reads==0&&
                      std::all_of(oldAudio.begin(),oldAudio.end(),[](float v){return v==0;}),
                  "A stale job accessed replacement audio or its new channel layout");
            oldAudio.fill(.75f);
            check(!reader.read(oldChannels,current,0,127)&&replacement.reads==0&&oldAudio.back()==0,
                  "A short destination span indexed the replacement source channels");
            reader.close();replacement.channels=1;
            check(reader.open(replacement.ref(),&replacement.api,replacement.source(),{48000.,1,100}),"Same-layout replacement open failed");
            check(!reader.read(oldChannels,old,0,127)&&replacement.reads==0,
                  "Same-layout replacement reused an earlier source revision");
            const auto ready=reader.snapshot();
            check(reader.read(oldChannels,ready,0,127)&&oldAudio[0]==sample(0,0)&&oldAudio[126]==0,
                  "Fresh snapshot failed after source replacement");
            Host otherHost;cell::ara::SourceReader other;
            check(other.open(otherHost.ref(),&otherHost.api,otherHost.source(),{48000.,1,100}),"Independent reader failed to open");
            check(!other.read(oldChannels,ready,0,127)&&otherHost.reads==0,
                  "A snapshot from a different source with identical metadata was accepted");
        }
        Host host; host.block = true;
        cell::ara::SourceReader reader;
        check(reader.open(host.ref(), &host.api, host.source(), {48000., 1, 50000}), "Blocking reader open failed");
        const auto snapshot=reader.snapshot();
        std::vector<float> pcm(32768, 1.f); float *output[] = {pcm.data()};
        auto read = std::async(std::launch::async, [&] { return reader.read(output, snapshot, 0, int(pcm.size())); });
        { std::unique_lock<std::mutex> lock(host.mutex);
          check(host.changed.wait_for(lock, 3s, [&] { return host.entered; }), "Host read did not start"); }
        auto closed = std::async(std::launch::async, [&] { reader.close(); });
        const bool premature = closed.wait_for(20ms) == std::future_status::ready;
        { const std::lock_guard<std::mutex> lock(host.mutex); host.release = true; }
        host.changed.notify_all();
        check(closed.wait_for(3s) == std::future_status::ready && read.wait_for(3s) == std::future_status::ready,
              "Host reader revocation did not join bounded work");
        closed.get();
        check(!premature && !read.get() && host.destroys == 1 && host.badDestroy == 0,
              "Host reader was destroyed during an in-flight read");
        check(std::all_of(pcm.begin(), pcm.end(), [](float x) { return x == 0.f; }), "Revoked source published a partial page");
        std::cout << "PASS native ARA C source: exact 1/2/32/64-channel PCM,64-bit positions,bounded allocation-free reads,padding,failure,joined revocation,source-bound snapshots and replacement channel-layout rejection\n";
    } catch (const std::exception &e) {
        watchAllocations = false;
        std::cerr << e.what() << '\n'; return 1;
    }
}
