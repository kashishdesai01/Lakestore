#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "lakestore/result.hpp"
namespace lakestore {
class ScanCancellation {
 public:
  void cancel() noexcept { cancelled_.store(true); }
  bool cancelled() const noexcept { return cancelled_.load(); }

 private:
  std::atomic<bool> cancelled_{false};
};
// Accounts conservatively for application-owned row-group buffers, not process RSS or metadata.
class ScanBudget {
 public:
  class Reservation {
   public:
    Reservation() = default;
    Reservation(ScanBudget* owner, uint64_t bytes) : owner_(owner), bytes_(bytes) {}
    Reservation(Reservation&& other) noexcept : owner_(other.owner_), bytes_(other.bytes_) {
      other.owner_ = nullptr;
    }
    Reservation(const Reservation&) = delete;
    ~Reservation() {
      if (owner_) owner_->release(bytes_);
    }
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    ScanBudget* owner_ = nullptr;
    uint64_t bytes_ = 0;
  };
  ScanBudget(uint64_t limit, ScanCancellation& cancellation)
      : limit_(limit), cancellation_(cancellation) {
    if (!limit || limit > 1024ULL * 1024 * 1024)
      fail(ErrorCode::InvalidArgument, "scan buffer budget must be 1 byte..1 GiB");
  }
  Reservation acquire(uint64_t bytes) {
    if (bytes > limit_) fail(ErrorCode::InvalidArgument, "row group exceeds scan buffer budget");
    std::unique_lock lock(mutex_);
    while (!cancellation_.cancelled() && bytes > limit_ - used_)
      ready_.wait_for(lock, std::chrono::milliseconds(10));
    if (cancellation_.cancelled()) return {};
    used_ += bytes;
    peak_ = std::max(peak_, used_);
    return {this, bytes};
  }
  uint64_t peak() const {
    std::lock_guard lock(mutex_);
    return peak_;
  }

 private:
  void release(uint64_t bytes) {
    {
      std::lock_guard lock(mutex_);
      used_ -= bytes;
    }
    ready_.notify_all();
  }
  uint64_t limit_, used_ = 0, peak_ = 0;
  ScanCancellation& cancellation_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
};
}  // namespace lakestore
