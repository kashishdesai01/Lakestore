#pragma once
#include <thread>
#include <utility>
namespace lakestore {
// Apple libc++ does not ship std::jthread on all supported SDKs.
class JoiningThread {
 public:
  template <class F>
  explicit JoiningThread(F&& fn) : thread_(std::forward<F>(fn)) {}
  JoiningThread(JoiningThread&& other) noexcept : thread_(std::move(other.thread_)) {}
  JoiningThread(const JoiningThread&) = delete;
  JoiningThread& operator=(const JoiningThread&) = delete;
  ~JoiningThread() {
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::thread thread_;
};
}  // namespace lakestore
