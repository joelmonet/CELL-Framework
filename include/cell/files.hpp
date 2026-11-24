#pragma once
#include <cell/core.hpp>
#include <filesystem>
#include <memory>
#include <cell/result.hpp>

namespace cell::files {
#if defined(_WIN32)
// Absolute extended Win32 path, including UNC. Does not impose MAX_PATH or
// require the host executable to provide a longPathAware manifest.
std::filesystem::path windowsPath(const std::filesystem::path&);
#endif
std::vector<std::byte> readBounded(const std::filesystem::path&,std::size_t limit);
// Streaming non-real-time output; destruction discards an uncommitted sibling.
class AtomicWriter {
public:
    explicit AtomicWriter(std::filesystem::path,bool replace=false);
    ~AtomicWriter();
    AtomicWriter(const AtomicWriter&)=delete;AtomicWriter& operator=(const AtomicWriter&)=delete;
    void write(std::span<const std::byte>);
    void commit();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Write a new sibling, flush, then replace. A failed write leaves the original
// preset untouched. Replacement requires an explicit caller decision.
void atomicWrite(const std::filesystem::path&,std::span<const std::byte>,bool replace=false);
// Closed regular file, same-directory publication. Exclusive publication never
// removes an existing destination; failure leaves the completed source intact.
Result publish(const std::filesystem::path&temporary,const std::filesystem::path&destination,bool replace);
}
