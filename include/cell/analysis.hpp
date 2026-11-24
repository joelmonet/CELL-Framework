#pragma once
#include <cell/core.hpp>
#include <cmath>

namespace cell {
inline constexpr std::size_t analysisSize = 1024;
inline constexpr std::uint32_t analysisVersion=4;
struct ModuleDisplay {
    std::uint32_t type=UINT32_MAX,count{};
    std::array<float,64> values{};
};
struct AnalysisFrame {
    std::uint32_t version = analysisVersion;
    std::uint32_t count = analysisSize;
    std::uint32_t latency{},flags{}; // bit 0: control-only host notification
    double sampleRate = 48000;
    std::uint64_t sequence{};
    float inputPeak{}, outputPeak{};
    std::array<float, analysisSize> dry{}, processed{};
    // Optional eight-stage input/output peaks and gain reduction, respectively.
    std::array<float,24> stages{};
    std::array<ModuleDisplay,8> modules{};
};
static_assert(sizeof(AnalysisFrame) == 136 + analysisSize * 8 + 8*sizeof(ModuleDisplay) && std::is_trivially_copyable_v<AnalysisFrame>);
inline constexpr std::uint32_t analysisContextId = 0x434c4134u;
inline constexpr const char* analysisEnableMessage = "Cell.Analysis.Enable.v4";
inline constexpr const char* analysisStatusMessage = "Cell.Analysis.Status.v4";
enum class AnalysisConnection : std::uint32_t { local, waiting, host, unavailable };

// Optional processor capability. One frame producer and one consumer. Display
// frames may be dropped under backpressure; parameter/control events may not.
class AnalysisSource {
public:
    virtual ~AnalysisSource() = default;
    virtual void enableAnalysis(bool) noexcept = 0;
    virtual bool analysisEnabled() const noexcept = 0;
    virtual bool pullAnalysis(AnalysisFrame&) noexcept = 0;
    virtual bool receiveAnalysis(const AnalysisFrame&) noexcept = 0;
    virtual void setAnalysisConnection(AnalysisConnection) noexcept = 0;
    virtual AnalysisConnection analysisConnection() const noexcept = 0;
    virtual std::uint64_t droppedAnalysisFrames() const noexcept = 0;
};

class AnalysisMailbox : public AnalysisSource {
public:
    void enableAnalysis(bool enabled) noexcept override { enabled_.store(enabled, std::memory_order_relaxed); }
    bool analysisEnabled() const noexcept override { return enabled_.load(std::memory_order_relaxed); }
    bool pullAnalysis(AnalysisFrame& frame) noexcept override { return frames_.pop(frame); }
    bool receiveAnalysis(const AnalysisFrame& frame) noexcept override {
        if (frame.version != analysisVersion || frame.flags!=0 || frame.count != analysisSize || !std::isfinite(frame.sampleRate) || frame.sampleRate < 1000 || frame.sampleRate > 768000 ||
            !std::isfinite(frame.inputPeak) || !std::isfinite(frame.outputPeak)) return false;
        for (std::size_t i = 0; i < analysisSize; ++i) if (!std::isfinite(frame.dry[i]) || !std::isfinite(frame.processed[i])) return false;
        for(float value:frame.stages)if(!std::isfinite(value))return false;
        for(const auto& module:frame.modules){
            if(module.count>module.values.size()||(module.count&&module.type>4095))return false;
            for(float value:module.values)if(!std::isfinite(value))return false;
        }
        return publish(frame);
    }
    void setAnalysisConnection(AnalysisConnection value) noexcept override { connection_.store(value, std::memory_order_relaxed); }
    AnalysisConnection analysisConnection() const noexcept override { return connection_.load(std::memory_order_relaxed); }
    std::uint64_t droppedAnalysisFrames() const noexcept override { return drops_.load(std::memory_order_relaxed); }
    bool publish(const AnalysisFrame& frame) noexcept {
        if (frames_.push(frame)) return true;
        drops_.fetch_add(1, std::memory_order_relaxed); return false;
    }
private:
    SpscQueue<AnalysisFrame, 4> frames_;
    std::atomic<bool> enabled_{false};
    std::atomic<AnalysisConnection> connection_{AnalysisConnection::local};
    std::atomic<std::uint64_t> drops_{};
};
}
