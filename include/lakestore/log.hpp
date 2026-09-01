#pragma once
#include <map>
#include <string_view>

#include "lakestore/format.hpp"
namespace lakestore {
void validate_table_id(const std::string&);
bool valid_data_path(std::string_view);
std::string log_key(const std::string&, uint64_t);
struct DataFile {
  std::string path;
  uint64_t size, rows;
  Json stats;
  bool operator==(const DataFile&) const = default;
};
Json file_json(const DataFile&);
DataFile file_from_json(const Json&);
struct Snapshot {
  uint64_t version = 0;
  int64_t timestamp_ms = 0;
  Schema schema;
  std::map<std::string, DataFile> files;
  uint64_t retention_floor = 1;
};
struct TxnMetrics {
  std::atomic<uint64_t> attempts{0}, conflicts{0}, resolutions{0};
};
class Log {
 public:
  explicit Log(std::shared_ptr<ObjectStore> store,
               std::shared_ptr<TxnMetrics> metrics = std::make_shared<TxnMetrics>(),
               unsigned checkpoint_every = 10)
      : store_(std::move(store)),
        metrics_(std::move(metrics)),
        checkpoint_every_(checkpoint_every) {}
  Snapshot latest(const std::string& table);
  Snapshot at(const std::string& table, uint64_t version);
  std::vector<Json> history(const std::string& table);
  uint64_t commit(const std::string& table, const Snapshot& base, Json actions,
                  const std::string& operation, bool strict = false,
                  const std::vector<std::string>& removals = {});
  void register_table(const std::string&);
  void checkpoint(const std::string&, const Snapshot&);
  const std::shared_ptr<TxnMetrics>& metrics() const { return metrics_; }

 private:
  Snapshot reconstruct(const std::string&, uint64_t);
  uint64_t latest_version(const std::string&);
  std::shared_ptr<ObjectStore> store_;
  std::shared_ptr<TxnMetrics> metrics_;
  unsigned checkpoint_every_;
};
}  // namespace lakestore
