#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <variant>

#include "lakestore/store.hpp"
#include "lakestore/util/scan_budget.hpp"
namespace lakestore {
using Json = nlohmann::json;
enum class Type { Int64, Double, String, Timestamp };
struct Column {
  std::string name;
  Type type;
  bool operator==(const Column&) const = default;
};
using Schema = std::vector<Column>;
using Value = std::variant<std::monostate, int64_t, double, std::string>;
using Row = std::vector<Value>;
using Rows = std::vector<Row>;
struct Predicate {
  size_t column;
  std::string op;
  Value literal;
};
Schema parse_schema(const std::string&);
Json schema_json(const Schema&);
Schema schema_from_json(const Json&);
Value parse_value(Type, const std::string&);
Json row_json(const Row&);
std::vector<Predicate> parse_predicates(const Schema&, const std::string&);
bool matches(const Row&, const std::vector<Predicate>&);
bool may_match(const Json& stats, const std::vector<Predicate>&);
uint32_t crc32c(std::string_view);
uint64_t json_uint(const Json&);
int64_t json_int(const Json&);
struct EncodedFile {
  std::string bytes;
  Json stats;
  uint64_t rows;
};
EncodedFile encode_file(const Schema&, const Rows&, size_t group_rows = 1024);
struct FileFooter {
  Schema schema;
  Json groups, stats;
  uint64_t rows, data_end;
};
FileFooter read_footer(ObjectStore&, const std::string& key);
struct ScanMetrics {
  uint64_t files_read = 0, files_pruned = 0, groups_pruned = 0, data_bytes_read = 0,
           bytes_skipped = 0;
};
// Sink receives a borrowed row-group batch. False requests cancellation; exceptions propagate.
using BatchSink = std::function<bool(const Rows&)>;
void stream_file_rows(ObjectStore&, const std::string&, const Schema&,
                      const std::vector<Predicate>&, const std::vector<size_t>&, ScanMetrics&,
                      bool prune, ScanBudget&, ScanCancellation&, const BatchSink&);
Rows read_file_rows(ObjectStore&, const std::string&, const Schema&, const std::vector<Predicate>&,
                    const std::vector<size_t>& projection, ScanMetrics&, bool prune = true);
Rows read_csv(const Schema&, const std::string& path);
}  // namespace lakestore
