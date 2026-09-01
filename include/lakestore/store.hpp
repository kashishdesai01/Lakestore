#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "lakestore/result.hpp"
namespace lakestore {
int64_t now_ms();
std::string uuid();
void validate_key(std::string_view key);
struct ObjectMeta {
  uint64_t size{};
  std::string etag;
  int64_t modified_ms{};
};
struct KeyInfo {
  std::string key;
  ObjectMeta meta;
};
class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  virtual Result<std::string> get(const std::string&) = 0;
  virtual Result<std::string> get_range(const std::string&, uint64_t, uint64_t) = 0;
  virtual Result<ObjectMeta> head(const std::string&) = 0;
  virtual Result<ObjectMeta> put(const std::string&, std::string_view) = 0;
  virtual Result<ObjectMeta> put_if_absent(const std::string&, std::string_view) = 0;
  virtual Result<ObjectMeta> put_if_match(const std::string&, std::string_view,
                                          const std::string&) = 0;
  virtual Status erase(const std::string&) = 0;
  // Complete listing, including all backend pages. No multi-call snapshot guarantee.
  virtual Result<std::vector<KeyInfo>> list(const std::string&) = 0;
};
class MemoryStore final : public ObjectStore {
 public:
  Result<std::string> get(const std::string&) override;
  Result<std::string> get_range(const std::string&, uint64_t, uint64_t) override;
  Result<ObjectMeta> head(const std::string&) override;
  Result<ObjectMeta> put(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_absent(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_match(const std::string&, std::string_view,
                                  const std::string&) override;
  Status erase(const std::string&) override;
  Result<std::vector<KeyInfo>> list(const std::string&) override;

 private:
  struct Object {
    std::string bytes;
    ObjectMeta meta;
  };
  Result<ObjectMeta> write(const std::string&, std::string_view, int, const std::string&);
  std::mutex mutex_;
  std::map<std::string, Object> objects_;
};
class LocalStore final : public ObjectStore {
 public:
  explicit LocalStore(std::filesystem::path root);
  Result<std::string> get(const std::string&) override;
  Result<std::string> get_range(const std::string&, uint64_t, uint64_t) override;
  Result<ObjectMeta> head(const std::string&) override;
  Result<ObjectMeta> put(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_absent(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_match(const std::string&, std::string_view,
                                  const std::string&) override;
  Status erase(const std::string&) override;
  Result<std::vector<KeyInfo>> list(const std::string&) override;

 private:
  Result<ObjectMeta> write(const std::string&, std::string_view, int, const std::string&);
  std::filesystem::path path(const std::string&) const;
  std::filesystem::path root_;
};
class StoreDecorator : public ObjectStore {
 public:
  explicit StoreDecorator(std::shared_ptr<ObjectStore> inner) : inner_(std::move(inner)) {}
  Result<std::string> get(const std::string& k) override { return inner_->get(k); }
  Result<std::string> get_range(const std::string& k, uint64_t o, uint64_t n) override {
    return inner_->get_range(k, o, n);
  }
  Result<ObjectMeta> head(const std::string& k) override { return inner_->head(k); }
  Result<ObjectMeta> put(const std::string& k, std::string_view v) override {
    return inner_->put(k, v);
  }
  Result<ObjectMeta> put_if_absent(const std::string& k, std::string_view v) override {
    return inner_->put_if_absent(k, v);
  }
  Result<ObjectMeta> put_if_match(const std::string& k, std::string_view v,
                                  const std::string& e) override {
    return inner_->put_if_match(k, v, e);
  }
  Status erase(const std::string& k) override { return inner_->erase(k); }
  Result<std::vector<KeyInfo>> list(const std::string& p) override { return inner_->list(p); }

 protected:
  std::shared_ptr<ObjectStore> inner_;
};
struct IoMetrics {
  std::atomic<uint64_t> requests{0}, bytes_read{0}, retries{0};
};
class RetryingStore final : public StoreDecorator {
 public:
  RetryingStore(std::shared_ptr<ObjectStore>, std::shared_ptr<IoMetrics>, unsigned attempts = 4,
                std::chrono::milliseconds delay = std::chrono::milliseconds(10));
  Result<std::string> get(const std::string&) override;
  Result<std::string> get_range(const std::string&, uint64_t, uint64_t) override;
  Result<ObjectMeta> head(const std::string&) override;
  Result<ObjectMeta> put(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_absent(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_match(const std::string&, std::string_view,
                                  const std::string&) override;
  Status erase(const std::string&) override;
  Result<std::vector<KeyInfo>> list(const std::string&) override;

 private:
  template <class F>
  auto retry(F fn) -> decltype(fn());
  std::shared_ptr<IoMetrics> metrics_;
  unsigned attempts_;
  std::chrono::milliseconds delay_;
};
struct Fault {
  std::string operation;
  std::string key_prefix;
  ErrorCode code;
  bool after_success = false;
};
class FaultInjectingStore final : public StoreDecorator {
 public:
  using StoreDecorator::StoreDecorator;
  void inject(Fault);
  // Hook runs outside the fault queue lock, useful for deterministic race barriers.
  void set_hook(std::function<void(std::string_view, const std::string&)> hook);
  Result<std::string> get(const std::string&) override;
  Result<std::string> get_range(const std::string&, uint64_t, uint64_t) override;
  Result<ObjectMeta> put(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_absent(const std::string&, std::string_view) override;
  Result<ObjectMeta> put_if_match(const std::string&, std::string_view,
                                  const std::string&) override;
  Result<std::vector<KeyInfo>> list(const std::string&) override;

 private:
  template <class F>
  auto run(std::string_view op, const std::string& key, F fn) -> decltype(fn());
  std::mutex mutex_;
  std::vector<Fault> faults_;
  std::function<void(std::string_view, const std::string&)> hook_;
};
#ifdef LAKESTORE_CLOUD
struct CloudConfig {
  enum class Provider { S3, Azure };
  Provider provider;
  std::string endpoint, container, prefix, region = "us-east-1";
  std::string access_key, secret_key, session_token, account, account_key, bearer_token;
  long timeout_ms = 30000;
};
std::shared_ptr<ObjectStore> cloud_store(CloudConfig);
Status initialize_cloud(CloudConfig);
#endif
}  // namespace lakestore
