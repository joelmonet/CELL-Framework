#pragma once
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define CELL_HAS_SSE_CONTROL 1
#endif

namespace cell {
// Preserve the host's FPU settings while preventing slow subnormal arithmetic.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if CELL_HAS_SSE_CONTROL
        saved_=_mm_getcsr();changed_=(saved_&0x8040u)!=0x8040u;
        if(changed_)_mm_setcsr(static_cast<unsigned>(saved_)|0x8040u);
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        const auto value=saved_|(std::uint64_t(1)<<24);changed_=value!=saved_;
        if(changed_)asm volatile("msr fpcr, %0" :: "r"(value));
#endif
    }
    ~ScopedNoDenormals() noexcept {
#if CELL_HAS_SSE_CONTROL
        if(changed_)_mm_setcsr(static_cast<unsigned>(saved_));
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
        if(changed_)asm volatile("msr fpcr, %0" :: "r"(saved_));
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&)=delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&)=delete;
private:
    std::uint64_t saved_{};
    bool changed_{};
};
}
