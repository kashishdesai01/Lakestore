#pragma once
#include "lakestore/log.hpp"
namespace lakestore {
struct ScanOptions {
  std::optional<uint64_t> version;
  std::optional<int64_t> as_of_ms;
  std::vector<Predicate> predicates;
  // Empty means all columns. Ordering and duplicates are preserved.
  std::vector<size_t> projection;
  size_t parallelism = 4;
  bool prune = true;
  uint64_t max_buffer_bytes = 64ULL * 1024 * 1024;
};
struct ScanResult {
  Rows rows;
  ScanMetrics metrics;
  uint64_t version;
};
struct ScanSummary {
  ScanMetrics metrics;
  uint64_t version = 0, rows_delivered = 0, peak_reserved_bytes = 0;
  bool cancelled = false;
};
struct GcResult {
  uint64_t candidates = 0, deleted = 0;
  std::vector<std::string> paths;
};
class TableStore {
 public:
  explicit TableStore(std::shared_ptr<ObjectStore> store, unsigned checkpoint_every = 10);
  uint64_t create(const std::string&, const Schema&);
  uint64_t append(const std::string&, const Rows&);
  uint64_t overwrite(const std::string&, const Rows&);
  uint64_t restore(const std::string&, uint64_t);
  uint64_t clone(const std::string& source, uint64_t version, const std::string& target);
  uint64_t compact(const std::string&);
  uint64_t expire_keep_last(const std::string&, uint64_t keep);
  uint64_t expire_older_than(const std::string&, int64_t timestamp_ms);
  Snapshot snapshot(const std::string&, std::optional<uint64_t> version = {});
  ScanResult scan(const std::string&, const ScanOptions& options = {});
  // Serialized callbacks execute on workers; batches live only until the callback returns.
  // Slow consumers apply backpressure directly. Cancellation joins all workers before returning.
  ScanSummary scan_stream(const std::string&, const BatchSink&, const ScanOptions& options = {},
                          ScanCancellation* cancellation = nullptr);
  std::vector<Json> history(const std::string&);
  void verify(const std::string&);
  // Destructive GC requires caller-guaranteed namespace-wide quiescence, including readers.
  // First invocation marks; later invocations sweep matured manifests after rechecking roots.
  GcResult gc(std::chrono::milliseconds grace, bool offline, bool dry_run = false);
  Log& log() { return log_; }
  const std::shared_ptr<IoMetrics>& io_metrics() const { return io_metrics_; }

 private:
  Snapshot scan_snapshot(const std::string&, const ScanOptions&);
  std::vector<DataFile> upload(const std::string&, const Schema&, const Rows&);
  uint64_t expire_floor(const std::string&, const Snapshot&, uint64_t);
  std::map<std::string, DataFile> roots();
  std::shared_ptr<ObjectStore> store_;
  std::shared_ptr<IoMetrics> io_metrics_;
  Log log_;
};
}  // namespace lakestore
