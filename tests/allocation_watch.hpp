#pragma once
#include <cstdlib>
#include <new>
#include <cstddef>
#if defined(CELL_TEST_TRACE_ALLOCATIONS) && defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <malloc.h>
#endif
#if defined(_MSC_VER)
#define CELL_TEST_NOINLINE __declspec(noinline)
#else
#define CELL_TEST_NOINLINE __attribute__((noinline))
#endif
namespace cell_test {
inline thread_local bool audio{};
inline thread_local unsigned allocations{},deletions{};
inline thread_local int failAfter=-1;
inline void traceAllocation()noexcept{
#if defined(CELL_TEST_TRACE_ALLOCATIONS) && defined(__linux__)
    static thread_local unsigned remaining=12;
    if(audio&&remaining&&std::getenv("CELL_TRACE_ALLOCATIONS")){--remaining;audio=false;void* frames[18];const auto count=backtrace(frames,18);backtrace_symbols_fd(frames,count,2);audio=true;}
#endif
}
inline void beforeAllocation(){
    if(audio){++allocations;traceAllocation();}
    if(failAfter==0)throw std::bad_alloc();
    if(failAfter>0)--failAfter;
}
}
CELL_TEST_NOINLINE void* operator new(std::size_t n){cell_test::beforeAllocation();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
CELL_TEST_NOINLINE void* operator new[](std::size_t n){return ::operator new(n);}
CELL_TEST_NOINLINE void operator delete(void* p)noexcept{if(p&&cell_test::audio)++cell_test::deletions;std::free(p);}
CELL_TEST_NOINLINE void operator delete[](void* p)noexcept{::operator delete(p);}
CELL_TEST_NOINLINE void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
CELL_TEST_NOINLINE void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
CELL_TEST_NOINLINE void* operator new(std::size_t n,const std::nothrow_t&)noexcept{try{return ::operator new(n);}catch(...){return nullptr;}}
CELL_TEST_NOINLINE void* operator new[](std::size_t n,const std::nothrow_t&)noexcept{return ::operator new(n,std::nothrow);}
CELL_TEST_NOINLINE void operator delete(void* p,const std::nothrow_t&)noexcept{::operator delete(p);}
CELL_TEST_NOINLINE void operator delete[](void* p,const std::nothrow_t&)noexcept{::operator delete(p);}
CELL_TEST_NOINLINE void* operator new(std::size_t n,std::align_val_t align){
    cell_test::beforeAllocation();void* p{};
#if defined(_WIN32)
    p=_aligned_malloc(n?n:1,std::size_t(align));
#else
    if(posix_memalign(&p,std::size_t(align),n?n:1)!=0)p=nullptr;
#endif
    if(p)return p;
    throw std::bad_alloc();
}
CELL_TEST_NOINLINE void* operator new[](std::size_t n,std::align_val_t align){return ::operator new(n,align);}
CELL_TEST_NOINLINE void operator delete(void* p,std::align_val_t)noexcept{
    if(p&&cell_test::audio)++cell_test::deletions;
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
CELL_TEST_NOINLINE void operator delete[](void* p,std::align_val_t a)noexcept{::operator delete(p,a);}
CELL_TEST_NOINLINE void operator delete(void* p,std::size_t,std::align_val_t a)noexcept{::operator delete(p,a);}
CELL_TEST_NOINLINE void operator delete[](void* p,std::size_t,std::align_val_t a)noexcept{::operator delete(p,a);}
CELL_TEST_NOINLINE void* operator new(std::size_t n,std::align_val_t a,const std::nothrow_t&)noexcept{try{return ::operator new(n,a);}catch(...){return nullptr;}}
CELL_TEST_NOINLINE void* operator new[](std::size_t n,std::align_val_t a,const std::nothrow_t&)noexcept{return ::operator new(n,a,std::nothrow);}
CELL_TEST_NOINLINE void operator delete(void* p,std::align_val_t a,const std::nothrow_t&)noexcept{::operator delete(p,a);}
CELL_TEST_NOINLINE void operator delete[](void* p,std::align_val_t a,const std::nothrow_t&)noexcept{::operator delete(p,a);}
