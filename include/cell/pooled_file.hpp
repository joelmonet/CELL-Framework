// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/streams.hpp>
#include <filesystem>
#include <memory>
namespace cell::files {
class ReadPool;
// Decoder cursors and metadata remain alive while their OS handles can be
// recycled. All operations are preparation/cache-worker operations, not live
// audio callback operations. Each decoder retains its own logical cursor.
class PooledInputStream final : public cell::InputStream {
public:
  PooledInputStream(const std::filesystem::path &, std::shared_ptr<ReadPool>);
  ~PooledInputStream();
  int read(void *, int) override;
  bool setPosition(std::int64_t) override;
  std::int64_t getPosition() const override;
  std::int64_t getTotalLength() const override;
  bool openedOk() const noexcept;
  bool isExhausted() const override;

private:
  friend class ReadPool;
  struct Source;
  std::unique_ptr<Source> source_;
  std::shared_ptr<ReadPool> pool_;
  std::int64_t position_{};
};
class ReadPool {
public:
  static constexpr unsigned capacity = 16;
  struct Statistics {
    unsigned opened{}, peak{};
    std::uint64_t opens{}, bytes{};
  };
  static std::shared_ptr<ReadPool> shared();
  static std::shared_ptr<ReadPool> create(unsigned handles = capacity);
  unsigned limit() const noexcept;
  ~ReadPool();
  // Control/worker inspection; never used by the live callback.
  Statistics statistics() const;

private:
  friend class PooledInputStream;
  explicit ReadPool(unsigned handles = capacity);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool admit(const PooledInputStream::Source &);
  int read(const PooledInputStream::Source &, std::int64_t, void *, int);
};
} // namespace cell::files
