#include "lakestore/util/thread.hpp"
#pragma once
#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "lakestore/result.hpp"
namespace lakestore {
// Fixed workers, at most one request per worker, no unbounded task queue.
template <class F>
void parallel_for(size_t count, size_t concurrency, F fn) {
  if (concurrency == 0 || concurrency > 64)
    fail(ErrorCode::InvalidArgument, "parallelism must be 1..64");
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::exception_ptr error;
  std::mutex mutex;
  {
    std::vector<JoiningThread> workers;
    for (size_t w = 0; w < std::min(count, concurrency); ++w)
      workers.emplace_back([&] {
        while (!stop.load()) {
          auto index = next.fetch_add(1);
          if (index >= count) return;
          try {
            fn(index);
          } catch (...) {
            {
              std::lock_guard lock(mutex);
              if (!error) error = std::current_exception();
            }
            stop = true;
            return;
          }
        }
      });
  }  // jthread joins before results or exceptions leave this function.
  if (error) std::rethrow_exception(error);
}
}  // namespace lakestore
