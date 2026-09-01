#include "lakestore/store.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
namespace lakestore {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::string uuid() {
  thread_local std::mt19937_64 rng(std::random_device{}());
  std::array<unsigned char, 16> b{};
  for (auto& x : b) x = static_cast<unsigned char>(rng());
  b[6] = static_cast<unsigned char>((b[6] & 15) | 64);
  b[8] = static_cast<unsigned char>((b[8] & 63) | 128);
  std::ostringstream s;
  s << std::hex << std::setfill('0');
  for (size_t i = 0; i < b.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s << '-';
    s << std::setw(2) << static_cast<unsigned>(b[i]);
  }
  return s.str();
}
void validate_key(std::string_view key) {
  if (key.empty() || key.front() == '/' || key.back() == '/')
    fail(ErrorCode::InvalidArgument, "invalid object key");
  size_t start = 0;
  while (start < key.size()) {
    auto end = key.find('/', start);
    if (end == std::string_view::npos) end = key.size();
    auto part = key.substr(start, end - start);
    if (part.empty() || part == "." || part == ".." || part.front() == '.')
      fail(ErrorCode::InvalidArgument, "unsafe object key");
    for (char raw : part) {
      auto c = static_cast<unsigned char>(raw);
      if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.'))
        fail(ErrorCode::InvalidArgument, "unsupported object key character");
    }
    start = end + 1;
  }
}
namespace {
Error missing(const std::string& k) { return {ErrorCode::NotFound, "object not found: " + k}; }
Error io_error() {
  return {ErrorCode::Fatal, "filesystem error: " + std::string(std::strerror(errno))};
}
template <class F>
auto guarded(F fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const Failure& e) {
    return e.error();
  } catch (const std::exception& e) {
    return Error{ErrorCode::Fatal, e.what()};
  }
}
void validate_prefix(const std::string& p) {
  if (!p.empty()) validate_key(p.back() == '/' ? p.substr(0, p.size() - 1) : p);
}
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {
    if (fd < 0) throw Failure(io_error());
  }
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};
class FileLock {
 public:
  FileLock(const std::filesystem::path& root, bool exclusive)
      : fd_(::open((root / ".lakestore-lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600)) {
    if (::flock(fd_.get(), exclusive ? LOCK_EX : LOCK_SH) != 0) throw Failure(io_error());
  }

 private:
  Fd fd_;
};
void sync_dir(const std::filesystem::path& p) {
  Fd fd(::open(p.c_str(), O_RDONLY | O_DIRECTORY));
  if (::fsync(fd.get()) != 0) throw Failure(io_error());
}
std::string read_file(const std::filesystem::path& p, uint64_t offset, uint64_t length) {
  Fd fd(::open(p.c_str(), O_RDONLY | O_NOFOLLOW));
  std::string bytes(static_cast<size_t>(length), '\0');
  size_t done = 0;
  while (done < bytes.size()) {
    auto n = ::pread(fd.get(), bytes.data() + done, bytes.size() - done,
                     static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) throw Failure(io_error());
    if (n == 0) fail(ErrorCode::Corruption, "truncated local object");
    done += static_cast<size_t>(n);
  }
  return bytes;
}
ObjectMeta local_meta(const std::filesystem::path& p) {
  struct stat st{};
  if (::lstat(p.c_str(), &st) != 0) {
    if (errno == ENOENT) fail(ErrorCode::NotFound, "object not found");
    throw Failure(io_error());
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 36) fail(ErrorCode::Corruption, "invalid local object");
#ifdef __APPLE__
  auto ms = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
  auto ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
  return {static_cast<uint64_t>(st.st_size - 36), read_file(p, 0, 36), ms};
}
}  // namespace
Result<std::string> MemoryStore::get(const std::string& k) {
  return guarded([&]() -> Result<std::string> {
    validate_key(k);
    std::lock_guard lock(mutex_);
    auto it = objects_.find(k);
    if (it == objects_.end()) return missing(k);
    return it->second.bytes;
  });
}
Result<std::string> MemoryStore::get_range(const std::string& k, uint64_t o, uint64_t n) {
  return guarded([&]() -> Result<std::string> {
    validate_key(k);
    std::lock_guard lock(mutex_);
    auto it = objects_.find(k);
    if (it == objects_.end()) return missing(k);
    if (o > it->second.bytes.size() || n > it->second.bytes.size() - o)
      return Error{ErrorCode::InvalidArgument, "range outside object"};
    return it->second.bytes.substr(static_cast<size_t>(o), static_cast<size_t>(n));
  });
}
Result<ObjectMeta> MemoryStore::head(const std::string& k) {
  return guarded([&]() -> Result<ObjectMeta> {
    validate_key(k);
    std::lock_guard lock(mutex_);
    auto it = objects_.find(k);
    if (it == objects_.end()) return missing(k);
    return it->second.meta;
  });
}
Result<ObjectMeta> MemoryStore::write(const std::string& k, std::string_view v, int condition,
                                      const std::string& etag) {
  return guarded([&]() -> Result<ObjectMeta> {
    validate_key(k);
    std::lock_guard lock(mutex_);
    auto it = objects_.find(k);
    if (condition == 1 && it != objects_.end())
      return Error{ErrorCode::AlreadyExists, "object already exists"};
    if (condition == 2 && (it == objects_.end() || it->second.meta.etag != etag))
      return Error{ErrorCode::PreconditionFailed, "etag mismatch"};
    ObjectMeta m{v.size(), uuid(), now_ms()};
    objects_[k] = {std::string(v), m};
    return m;
  });
}
Result<ObjectMeta> MemoryStore::put(const std::string& k, std::string_view v) {
  return write(k, v, 0, {});
}
Result<ObjectMeta> MemoryStore::put_if_absent(const std::string& k, std::string_view v) {
  return write(k, v, 1, {});
}
Result<ObjectMeta> MemoryStore::put_if_match(const std::string& k, std::string_view v,
                                             const std::string& e) {
  return write(k, v, 2, e);
}
Status MemoryStore::erase(const std::string& k) {
  return guarded([&]() -> Status {
    validate_key(k);
    std::lock_guard lock(mutex_);
    objects_.erase(k);
    return ok();
  });
}
Result<std::vector<KeyInfo>> MemoryStore::list(const std::string& p) {
  return guarded([&]() -> Result<std::vector<KeyInfo>> {
    validate_prefix(p);
    std::lock_guard lock(mutex_);
    std::vector<KeyInfo> result;
    for (auto& [k, v] : objects_)
      if (k.starts_with(p)) result.push_back({k, v.meta});
    return result;
  });
}
LocalStore::LocalStore(std::filesystem::path root) {
  std::filesystem::create_directories(root);
  root_ = std::filesystem::canonical(root);
  std::filesystem::create_directories(root_ / "objects");
}
std::filesystem::path LocalStore::path(const std::string& k) const {
  validate_key(k);
  auto p = root_ / "objects" / k;
  auto part = root_;
  for (const auto& component : p.lexically_relative(root_)) {
    part /= component;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(part)))
      fail(ErrorCode::InvalidArgument, "symlink in object path");
  }
  return p;
}
Result<std::string> LocalStore::get(const std::string& k) {
  return guarded([&]() -> Result<std::string> {
    FileLock lock(root_, false);
    auto p = path(k);
    auto m = local_meta(p);
    return read_file(p, 36, m.size);
  });
}
Result<std::string> LocalStore::get_range(const std::string& k, uint64_t o, uint64_t n) {
  return guarded([&]() -> Result<std::string> {
    FileLock lock(root_, false);
    auto p = path(k);
    auto m = local_meta(p);
    if (o > m.size || n > m.size - o)
      return Error{ErrorCode::InvalidArgument, "range outside object"};
    return read_file(p, 36 + o, n);
  });
}
Result<ObjectMeta> LocalStore::head(const std::string& k) {
  return guarded([&]() -> Result<ObjectMeta> {
    FileLock lock(root_, false);
    return local_meta(path(k));
  });
}
Result<ObjectMeta> LocalStore::write(const std::string& k, std::string_view v, int condition,
                                     const std::string& etag) {
  return guarded([&]() -> Result<ObjectMeta> {
    FileLock lock(root_, true);
    auto p = path(k);
    bool exists = std::filesystem::exists(p);
    if (condition == 1 && exists) return Error{ErrorCode::AlreadyExists, "object already exists"};
    if (condition == 2 && (!exists || local_meta(p).etag != etag))
      return Error{ErrorCode::PreconditionFailed, "etag mismatch"};
    std::filesystem::create_directories(p.parent_path());
    auto id = uuid();
    auto temp = p.parent_path() / (".stage-" + id);
    bool published = false;
    try {
      Fd fd(::open(temp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600));
      for (auto bytes : {std::string_view(id), v}) {
        size_t done = 0;
        while (done < bytes.size()) {
          auto n = ::write(fd.get(), bytes.data() + done, bytes.size() - done);
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0) throw Failure(io_error());
          done += static_cast<size_t>(n);
        }
      }
      if (::fsync(fd.get()) != 0) throw Failure(io_error());
      if (::rename(temp.c_str(), p.c_str()) != 0) throw Failure(io_error());
      published = true;
      // Sync all ancestors: a successful first write must also persist newly created directories.
      for (auto dir = p.parent_path();; dir = dir.parent_path()) {
        sync_dir(dir);
        if (dir == root_) break;
      }
      return local_meta(p);
    } catch (...) {
      std::error_code ec;
      std::filesystem::remove(temp, ec);
      if (published)
        return Error{ErrorCode::Timeout, "local publication outcome uncertain after rename"};
      throw;
    }
  });
}
Result<ObjectMeta> LocalStore::put(const std::string& k, std::string_view v) {
  return write(k, v, 0, {});
}
Result<ObjectMeta> LocalStore::put_if_absent(const std::string& k, std::string_view v) {
  return write(k, v, 1, {});
}
Result<ObjectMeta> LocalStore::put_if_match(const std::string& k, std::string_view v,
                                            const std::string& e) {
  return write(k, v, 2, e);
}
Status LocalStore::erase(const std::string& k) {
  return guarded([&]() -> Status {
    FileLock lock(root_, true);
    auto p = path(k);
    if (std::filesystem::remove(p)) sync_dir(p.parent_path());
    return ok();
  });
}
Result<std::vector<KeyInfo>> LocalStore::list(const std::string& prefix) {
  return guarded([&]() -> Result<std::vector<KeyInfo>> {
    validate_prefix(prefix);
    FileLock lock(root_, false);
    std::vector<KeyInfo> result;
    for (auto it = std::filesystem::recursive_directory_iterator(root_ / "objects");
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it->path().filename().string().starts_with('.')) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      if (it->is_symlink()) fail(ErrorCode::Corruption, "symlink in object namespace");
      if (it->is_regular_file()) {
        auto k = it->path().lexically_relative(root_ / "objects").generic_string();
        if (k.starts_with(prefix)) result.push_back({k, local_meta(it->path())});
      }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    return result;
  });
}
RetryingStore::RetryingStore(std::shared_ptr<ObjectStore> inner, std::shared_ptr<IoMetrics> m,
                             unsigned attempts, std::chrono::milliseconds d)
    : StoreDecorator(std::move(inner)), metrics_(std::move(m)), attempts_(attempts), delay_(d) {
  if (attempts == 0 || attempts > 16 || d.count() < 0)
    fail(ErrorCode::InvalidArgument, "invalid retry policy");
}
template <class F>
auto RetryingStore::retry(F fn) -> decltype(fn()) {
  thread_local std::mt19937 rng(std::random_device{}());
  for (unsigned i = 0;; ++i) {
    ++metrics_->requests;
    auto result = fn();
    if (result ||
        (result.error().code != ErrorCode::Timeout &&
         result.error().code != ErrorCode::Transient) ||
        i + 1 >= attempts_)
      return result;
    ++metrics_->retries;
    auto cap = delay_.count() * (int64_t{1} << i);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::uniform_int_distribution<int64_t>(0, cap)(rng)));
  }
}
Result<std::string> RetryingStore::get(const std::string& k) {
  auto r = retry([&] { return inner_->get(k); });
  if (r) metrics_->bytes_read += r.value().size();
  return r;
}
Result<std::string> RetryingStore::get_range(const std::string& k, uint64_t o, uint64_t n) {
  auto r = retry([&] { return inner_->get_range(k, o, n); });
  if (r) metrics_->bytes_read += r.value().size();
  return r;
}
Result<ObjectMeta> RetryingStore::head(const std::string& k) {
  return retry([&] { return inner_->head(k); });
}
Result<ObjectMeta> RetryingStore::put(const std::string& k, std::string_view v) {
  return retry([&] { return inner_->put(k, v); });
}
// Conditional writes are owned by the transaction protocol, never automatically retried.
Result<ObjectMeta> RetryingStore::put_if_absent(const std::string& k, std::string_view v) {
  ++metrics_->requests;
  return inner_->put_if_absent(k, v);
}
Result<ObjectMeta> RetryingStore::put_if_match(const std::string& k, std::string_view v,
                                               const std::string& e) {
  ++metrics_->requests;
  return inner_->put_if_match(k, v, e);
}
Status RetryingStore::erase(const std::string& k) {
  return retry([&] { return inner_->erase(k); });
}
Result<std::vector<KeyInfo>> RetryingStore::list(const std::string& p) {
  return retry([&] { return inner_->list(p); });
}
void FaultInjectingStore::inject(Fault f) {
  std::lock_guard lock(mutex_);
  faults_.push_back(std::move(f));
}
void FaultInjectingStore::set_hook(std::function<void(std::string_view, const std::string&)> h) {
  std::lock_guard lock(mutex_);
  hook_ = std::move(h);
}
template <class F>
auto FaultInjectingStore::run(std::string_view op, const std::string& key, F fn) -> decltype(fn()) {
  std::optional<Fault> fault;
  std::function<void(std::string_view, const std::string&)> hook;
  {
    std::lock_guard lock(mutex_);
    hook = hook_;
    auto it = std::find_if(faults_.begin(), faults_.end(), [&](auto& f) {
      return f.operation == op && key.starts_with(f.key_prefix);
    });
    if (it != faults_.end()) {
      fault = *it;
      faults_.erase(it);
    }
  }
  if (hook) hook(op, key);
  if (fault && !fault->after_success) return Error{fault->code, "injected failure"};
  auto result = fn();
  if (fault && result) return Error{fault->code, "injected failure after success"};
  return result;
}
Result<std::string> FaultInjectingStore::get(const std::string& k) {
  return run("get", k, [&] { return inner_->get(k); });
}
Result<std::string> FaultInjectingStore::get_range(const std::string& k, uint64_t o, uint64_t n) {
  return run("get_range", k, [&] { return inner_->get_range(k, o, n); });
}
Result<ObjectMeta> FaultInjectingStore::put(const std::string& k, std::string_view v) {
  return run("put", k, [&] { return inner_->put(k, v); });
}
Result<ObjectMeta> FaultInjectingStore::put_if_absent(const std::string& k, std::string_view v) {
  return run("put_if_absent", k, [&] { return inner_->put_if_absent(k, v); });
}
Result<ObjectMeta> FaultInjectingStore::put_if_match(const std::string& k, std::string_view v,
                                                     const std::string& e) {
  return run("put_if_match", k, [&] { return inner_->put_if_match(k, v, e); });
}
Result<std::vector<KeyInfo>> FaultInjectingStore::list(const std::string& p) {
  return run("list", p, [&] { return inner_->list(p); });
}
}  // namespace lakestore
