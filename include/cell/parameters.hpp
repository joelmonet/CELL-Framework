#pragma once

#include <cell/core.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cell {

using ParamId = std::uint32_t;

struct Parameter {
    ParamId id{};
    std::string key;
    std::string name;
    std::string unit;
    double minimum{};
    double maximum = 1.0;
    double defaultValue{};
    double step{};
    double skew = 1.0;
    bool automatable = true;
    bool bypass{};
    // Immutable presentation metadata. Values and serialized parameter IDs
    // remain numeric; audio bindings never copy these labels.
    std::vector<std::string> choices;

    double clamp(double value) const noexcept {
        if (!std::isfinite(value)) return defaultValue;
        // The declared default is authoritative. Re-snapping a float-authored
        // interval in double precision can move an exact centre off zero.
        if (value == defaultValue) return defaultValue;
        value = std::clamp(value, minimum, maximum);
        if (step > 0) {
            // Round the product before adding the minimum, as in released
            // state. Fusing these operations can change saved values on ARM.
            const volatile double offset = std::round((value - minimum) / step) * step;
            value = minimum + offset;
        }
        return std::clamp(value, minimum, maximum);
    }
    double fromNormalized(double value) const noexcept {
        if (!std::isfinite(value)) return defaultValue;
        return clamp(minimum + (maximum - minimum) * std::pow(std::clamp(value, 0.0, 1.0), 1.0 / skew));
    }
    double toNormalized(double value) const noexcept {
        return std::pow((clamp(value) - minimum) / (maximum - minimum), skew);
    }
    std::string text(double value) const {
        if (!choices.empty()) {
            const auto index=clamp(value)-minimum;
            if(index>=0&&index<double(choices.size())&&index==std::floor(index))return choices[std::size_t(index)];
        }
        char result[96]{};
        std::snprintf(result, sizeof(result), step >= 1 ? "%.0f%s%s" : "%.2f%s%s",
                      clamp(value), unit.empty() ? "" : " ", unit.c_str());
        return result;
    }
    // Control thread parameter text input.
    std::optional<double> parseText(std::string_view) const;
};

class ParameterBank {
public:
    // Resolve during preparation. Bindings remain valid for the bank's
    // lifetime and preserve the ordinary validation/atomic ordering rules.
    class ReadBinding {
    public:
        ReadBinding() = default;
        explicit operator bool() const noexcept { return value_ != nullptr; }
        const Parameter* descriptor() const noexcept { return field_; }
        double load() const noexcept { return value_ ? value_->load(std::memory_order_relaxed) : 0.; }
        double acquire() const noexcept { return value_ ? value_->load(std::memory_order_acquire) : 0.; }
    protected:
        friend class ParameterBank;
        ReadBinding(const Parameter* field,const std::atomic<double>* value) : field_(field), value_(value) {}
    private:
        const Parameter* field_{};
        const std::atomic<double>* value_{};
    };
    class Binding : public ReadBinding {
    public:
        Binding() = default;
        bool set(double value) const noexcept {
            if (!write_ || !std::isfinite(value)) return false;
            write_->store(descriptor()->clamp(value),std::memory_order_relaxed); return true;
        }
        bool restorePlain(double value) const noexcept {
            const auto* field = descriptor();
            if (!write_ || !std::isfinite(value) || value < field->minimum || value > field->maximum ||
                (field->step >= 1 && field->clamp(value) != value)) return false;
            write_->store(value,std::memory_order_release); return true;
        }
    private:
        friend class ParameterBank;
        Binding(const Parameter* field,std::atomic<double>* value) : ReadBinding(field,value),write_(value) {}
        std::atomic<double>* write_{};
    };
    explicit ParameterBank(std::span<const Parameter> parameters) : parameters_(parameters.begin(),parameters.end()),
        values_(std::make_unique<std::atomic<double>[]>(parameters.size())) {
        static_assert(std::atomic<double>::is_always_lock_free);
        lookup_.reserve(parameters_.size());
        std::vector<std::string_view> keys; keys.reserve(parameters_.size());
        for (std::size_t i = 0; i < parameters_.size(); ++i) {
            const auto& p = parameters_[i];
            if (p.id == UINT32_MAX || p.key.empty() || p.name.empty() || !std::isfinite(p.minimum) ||
                !std::isfinite(p.maximum) || p.maximum <= p.minimum || !std::isfinite(p.defaultValue) ||
                p.defaultValue < p.minimum || p.defaultValue > p.maximum ||
                !std::isfinite(p.skew) || p.skew <= 0 || !std::isfinite(p.step) || p.step < 0)
                throw std::invalid_argument("invalid parameter descriptor: " + p.key);
            if(!p.choices.empty()&&(p.step!=1||p.minimum!=std::floor(p.minimum)||p.maximum-p.minimum+1!=double(p.choices.size())||std::any_of(p.choices.begin(),p.choices.end(),[](const auto& label){return label.size()>4096||label.find(char(0))!=std::string::npos;})))throw std::invalid_argument("invalid parameter choices: "+p.key);
            values_[i].store(p.clamp(p.defaultValue), std::memory_order_relaxed);
            lookup_.push_back({p.id, i});
            keys.push_back(p.key);
        }
        std::sort(lookup_.begin(),lookup_.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        for (std::size_t i = 1; i < lookup_.size(); ++i) if (lookup_[i-1].id == lookup_[i].id)
            throw std::invalid_argument("duplicate parameter ID: " + parameters_[lookup_[i].index].key);
        std::sort(keys.begin(), keys.end());
        for (std::size_t i = 1; i < keys.size(); ++i) if (keys[i-1] == keys[i])
            throw std::invalid_argument("duplicate parameter key: " + std::string(keys[i]));
    }
    std::span<const Parameter> descriptors() const noexcept { return parameters_; }
    ReadBinding bind(ParamId id) const noexcept {
        const auto index = indexOf(id);
        return index < parameters_.size() ? ReadBinding(&parameters_[index],&values_[index]) : ReadBinding{};
    }
    Binding bind(ParamId id) noexcept {
        const auto index = indexOf(id);
        return index < parameters_.size() ? Binding(&parameters_[index],&values_[index]) : Binding{};
    }
    // Descriptor-order snapshot without N ID searches. Each value is atomic;
    // products use their publication generation for a multi-value transaction.
    bool copyValues(std::span<double> output, bool acquire = false) const noexcept {
        if (output.size() != parameters_.size()) return false;
        const auto order = acquire ? std::memory_order_acquire : std::memory_order_relaxed;
        for (std::size_t i = 0; i < output.size(); ++i) output[i] = values_[i].load(order);
        return true;
    }
    const Parameter* find(ParamId id) const noexcept {
        const auto index=indexOf(id);
        return index<parameters_.size()?&parameters_[index]:nullptr;
    }
    double get(ParamId id) const noexcept {
        const auto index=indexOf(id);
        return index<parameters_.size()?values_[index].load(std::memory_order_relaxed):0.0;
    }
    // Pair with restorePlain's release store when a product validates a
    // multi-value snapshot against its own publication generation.
    double getAcquire(ParamId id)const noexcept{
        const auto index=indexOf(id);
        return index<parameters_.size()?values_[index].load(std::memory_order_acquire):0.0;
    }
    bool set(ParamId id, double value) noexcept {
        if (!std::isfinite(value)) return false;
        const auto index=indexOf(id);
        if(index>=parameters_.size())return false;
        values_[index].store(parameters_[index].clamp(value),std::memory_order_relaxed);
        return true;
    }
    // Non-audio migration/state commit of already normalized/validated plain
    // values. Preserve float32 reference roundoff instead of snapping it again.
    bool restorePlain(ParamId id, double value) noexcept {
        const auto index=indexOf(id);
        if(index>=parameters_.size()||!std::isfinite(value))return false;
        const auto& p=parameters_[index];
        if(value<p.minimum||value>p.maximum||(p.step>=1&&p.clamp(value)!=value))return false;
        values_[index].store(value,std::memory_order_release);return true;
    }
private:
    struct Lookup {ParamId id;std::size_t index;};
public:
    // Stable descriptor-order index for formats whose automation ABI uses
    // indexes. An unknown ID returns descriptors().size().
    std::size_t indexOf(ParamId id)const noexcept {
        const auto found=std::lower_bound(lookup_.begin(),lookup_.end(),id,[](const Lookup& item,ParamId value){return item.id<value;});
        return found!=lookup_.end()&&found->id==id?found->index:parameters_.size();
    }
private:
    std::vector<Parameter> parameters_;
    std::vector<Lookup> lookup_;
    std::unique_ptr<std::atomic<double>[]> values_;
};

enum class EditType : std::uint8_t { begin, value, end };
struct ParameterEvent { std::uint32_t frame{}; ParamId id{}; double value{}; EditType type{EditType::value}; };
// Stable merge ordering with caller-owned scratch: duplicate timestamps retain
// host order without the temporary allocation used by std::stable_sort.
template<class Event> inline bool stableFrameSort(std::span<Event> events,std::span<Event> scratch) noexcept {
    if(scratch.size()<events.size()||(!events.empty()&&events.data()==scratch.data()))return false;
    auto source=events;auto target=scratch.first(events.size());
    for(std::size_t width=1;width<events.size();width=width>=events.size()-width?events.size():width*2){
        for(std::size_t start=0;start<events.size();start+=std::min(events.size()-start,width)*2){
            const auto middle=start+std::min(width,events.size()-start),end=middle+std::min(width,events.size()-middle);
            auto a=start,b=middle;
            for(auto out=start;out<end;++out)target[out]=b==end||(a<middle&&source[a].frame<=source[b].frame)?source[a++]:source[b++];
        }
        std::swap(source,target);
    }
    if(source.data()!=events.data())std::copy(source.begin(),source.end(),events.begin());
    return true;
}
inline bool stableEventSort(std::span<ParameterEvent> events,std::span<ParameterEvent> scratch) noexcept{return stableFrameSort<ParameterEvent>(events,scratch);}
struct ParameterEdit { EditType type{}; ParamId id{}; double value{}; };

} // namespace cell
