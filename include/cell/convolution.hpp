#pragma once

#include <cell/dsp.hpp>
#include <memory>

namespace cell::dsp {

// Zero-latency mono FIR convolution: a direct head plus an FFT-partitioned tail.
// Kernel loading/preparation is non-realtime. Once prepared, process() and reset()
// neither allocate nor lock. Stream boundaries are independent of host block size.
class Convolver {
public:
    Convolver()=default;
    Convolver(Convolver&&)noexcept=default;
    Convolver& operator=(Convolver&&)noexcept=default;
    Convolver(const Convolver&)=delete;
    Convolver& operator=(const Convolver&)=delete;
    void prepare(std::span<const float> impulse,std::uint32_t partition=64) {
        Convolver candidate;
        candidate.build(impulse,partition);
        *this=std::move(candidate);
    }
private:
    void build(std::span<const float> impulse,std::uint32_t partition) {
        if(impulse.empty()||impulse.size()>(1u<<28)||partition<8||partition>4096||(partition&(partition-1)))
            throw std::invalid_argument("invalid convolution kernel or partition size");
        for(float sample:impulse)if(!std::isfinite(sample))throw std::invalid_argument("non-finite impulse response");
        partition_=partition;length_=impulse.size();
        // Keep the declared tail for host/lifecycle reporting, but do not
        // convolve exact padding zeros (not a thresholded tail truncation).
        while(impulse.size()>1&&impulse.back()==0)impulse=impulse.first(impulse.size()-1);
        effectiveLength_=impulse.size();head_.assign(impulse.begin(),impulse.begin()+std::min<std::size_t>(partition,impulse.size()));
        tailPartitions_=impulse.size()>partition?(impulse.size()-partition+partition-1)/partition:0;
        ring_.assign(partition,0);input_.assign(tailPartitions_?partition:0,0);tail_.assign(tailPartitions_?partition:0,0);overlap_.assign(tailPartitions_?partition:0,0);
        if(tailPartitions_)fft_=std::make_unique<FFT>(partition*2);work_.assign(tailPartitions_?partition*2:0,{});
        kernels_.assign(tailPartitions_*partition*2,{});history_.assign(kernels_.size(),{});
        for(std::size_t k=0;k<tailPartitions_;++k) {
            auto spectrum=std::span(kernels_).subspan(k*partition*2,partition*2);
            const auto begin=(k+1)*partition;
            for(std::size_t n=0;n<partition&&begin+n<impulse.size();++n)spectrum[n]=impulse[begin+n];
            fft_->transform(spectrum);
        }
        reset();
    }
public:
    void reset() noexcept {
        std::fill(ring_.begin(),ring_.end(),0);std::fill(input_.begin(),input_.end(),0);
        std::fill(tail_.begin(),tail_.end(),0);std::fill(overlap_.begin(),overlap_.end(),0);
        std::fill(history_.begin(),history_.end(),std::complex<float>{});
        ringPosition_=blockPosition_=historyPosition_=0;
    }
    float process(float sample) noexcept {
        if(head_.empty())return sample;
        ring_[ringPosition_]=sample;double direct=0;
        auto index=ringPosition_;
        for(float coefficient:head_){direct+=double(coefficient)*ring_[index];index=index?index-1:partition_-1;}
        ringPosition_=(ringPosition_+1)%partition_;
        const float output=float(direct)+(tailPartitions_?tail_[blockPosition_]:0.f);
        if(tailPartitions_)input_[blockPosition_]=sample;
        if(++blockPosition_==partition_){if(tailPartitions_)calculateTail();blockPosition_=0;}
        return output;
    }
    void process(std::span<float> samples) noexcept {for(auto& sample:samples)sample=process(sample);}
    std::uint32_t latency()const noexcept{return 0;}
    std::size_t tailSamples()const noexcept{return length_?length_-1:0;}
    std::size_t processingSamples()const noexcept{return effectiveLength_;}
private:
    void calculateTail() noexcept {
        const auto size=partition_*2;
        auto current=std::span(history_).subspan(historyPosition_*size,size);
        for(std::size_t i=0;i<partition_;++i)current[i]=input_[i];
        std::fill(current.begin()+partition_,current.end(),std::complex<float>{});fft_->transform(current);
        std::fill(work_.begin(),work_.end(),std::complex<float>{});
        for(std::size_t k=0;k<tailPartitions_;++k) {
            const auto offset=((historyPosition_+tailPartitions_-k)%tailPartitions_)*size;
            const auto kernel=k*size;
            for(std::size_t bin=0;bin<size;++bin)work_[bin]+=history_[offset+bin]*kernels_[kernel+bin];
        }
        fft_->transform(work_,true);
        for(std::size_t i=0;i<partition_;++i){tail_[i]=work_[i].real()+overlap_[i];overlap_[i]=work_[i+partition_].real();}
        historyPosition_=(historyPosition_+1)%tailPartitions_;
    }
    std::uint32_t partition_=64,ringPosition_{},blockPosition_{};
    std::size_t length_{},effectiveLength_{},tailPartitions_{},historyPosition_{};
    std::vector<float> head_,ring_,input_,tail_,overlap_;
    std::vector<std::complex<float>> kernels_,history_,work_;
    std::unique_ptr<FFT> fft_;
};

} // namespace cell::dsp
