#include "lakestore/table.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#include "lakestore/util/parallel.hpp"
namespace lakestore {
namespace {
Json adds(const std::vector<DataFile>& files) {
  Json a = Json::array();
  for (auto& f : files) a.push_back({{"add", file_json(f)}});
  return a;
}
std::vector<size_t> all_columns(const Schema& s) {
  std::vector<size_t> p(s.size());
  std::iota(p.begin(), p.end(), 0);
  return p;
}
Json checked_json(const std::string& s) {
  try {
    return Json::parse(s);
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "malformed GC/catalog metadata");
  }
}
}  // namespace
TableStore::TableStore(std::shared_ptr<ObjectStore> store, unsigned checkpoint_every)
    : io_metrics_(std::make_shared<IoMetrics>()),
      log_(std::make_shared<RetryingStore>(store, io_metrics_), std::make_shared<TxnMetrics>(),
           checkpoint_every) {
  store_ = std::make_shared<RetryingStore>(std::move(store), io_metrics_);
}
Snapshot TableStore::snapshot(const std::string& t, std::optional<uint64_t> v) {
  return v ? log_.at(t, *v) : log_.latest(t);
}
uint64_t TableStore::create(const std::string& t, const Schema& s) {
  auto metadata = schema_json(s);
  log_.register_table(t);
  return log_.commit(t, {}, Json::array({Json{{"metadata", metadata}}}), "create", true);
}
std::vector<DataFile> TableStore::upload(const std::string& t, const Schema& s, const Rows& rows) {
  constexpr size_t file_rows = 8192;
  auto count = (rows.size() + file_rows - 1) / file_rows;
  std::vector<DataFile> files(count);
  parallel_for(count, 4, [&](size_t i) {
    auto begin = i * file_rows, end = std::min(rows.size(), begin + file_rows);
    Rows slice(rows.begin() + static_cast<std::ptrdiff_t>(begin),
               rows.begin() + static_cast<std::ptrdiff_t>(end));
    auto f = encode_file(s, slice);
    auto key = "tables/" + t + "/data/" + uuid() + ".lsf";
    // Never overwrite a data key, even in the astronomically unlikely event of a UUID collision.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      auto r = store_->put_if_absent(key, f.bytes);
      if (r) break;
      if (r.error().code == ErrorCode::AlreadyExists ||
          r.error().code == ErrorCode::PreconditionFailed || r.error().code == ErrorCode::Timeout ||
          r.error().code == ErrorCode::Transient) {
        auto existing = store_->get(key);
        if (existing) {
          if (existing.value() != f.bytes) fail(ErrorCode::Corruption, "data UUID collision");
          break;
        }
        if (existing.error().code != ErrorCode::NotFound &&
            existing.error().code != ErrorCode::Timeout &&
            existing.error().code != ErrorCode::Transient)
          throw Failure(existing.error());
        if (attempt == 7) fail(ErrorCode::UnknownOutcome, "data upload outcome unknown: " + key);
      } else
        throw Failure(r.error());
    }
    files[i] = {key, f.bytes.size(), f.rows, std::move(f.stats)};
  });
  return files;
}
uint64_t TableStore::append(const std::string& t, const Rows& rows) {
  auto base = snapshot(t);
  auto files = upload(t, base.schema, rows);
  return log_.commit(t, base, adds(files), "append");
}
uint64_t TableStore::overwrite(const std::string& t, const Rows& rows) {
  auto base = snapshot(t);
  auto files = upload(t, base.schema, rows);
  Json actions = Json::array();
  for (auto& [path, _] : base.files) actions.push_back({{"remove", path}});
  for (auto& a : adds(files)) actions.push_back(a);
  return log_.commit(t, base, std::move(actions), "overwrite", true);
}
uint64_t TableStore::restore(const std::string& t, uint64_t v) {
  auto base = snapshot(t);
  auto target = snapshot(t, v);
  Json actions = Json::array();
  for (auto& [path, _] : base.files)
    if (!target.files.contains(path)) actions.push_back({{"remove", path}});
  for (auto& [path, f] : target.files)
    if (!base.files.contains(path)) actions.push_back({{"add", file_json(f)}});
  return log_.commit(t, base, std::move(actions), "restore", true);
}
uint64_t TableStore::clone(const std::string& source, uint64_t version, const std::string& target) {
  if (source == target) fail(ErrorCode::InvalidArgument, "clone requires a different target");
  auto s = snapshot(source, version);
  log_.register_table(target);
  Json actions = Json::array({Json{{"metadata", schema_json(s.schema)}},
                              Json{{"clone_source", {{"table", source}, {"version", version}}}}});
  for (auto& [_, f] : s.files) actions.push_back({{"add", file_json(f)}});
  return log_.commit(target, {}, std::move(actions), "clone", true);
}
uint64_t TableStore::compact(const std::string& t) {
  auto base = snapshot(t);
  ScanOptions options;
  options.version = base.version;
  auto rows = scan(t, options).rows;
  auto files = upload(t, base.schema, rows);
  Json actions = Json::array();
  std::vector<std::string> removals;
  for (auto& [path, _] : base.files) {
    actions.push_back({{"remove", path}});
    removals.push_back(path);
  }
  for (auto& a : adds(files)) actions.push_back(a);
  return log_.commit(t, base, std::move(actions), "compact", false, removals);
}
uint64_t TableStore::expire_floor(const std::string& t, const Snapshot& base, uint64_t floor) {
  return log_.commit(t, base,
                     Json::array({Json{{"expire_before", std::max(floor, base.retention_floor)}}}),
                     "expire", true);
}
uint64_t TableStore::expire_keep_last(const std::string& t, uint64_t keep) {
  if (keep == 0) fail(ErrorCode::InvalidArgument, "keep-last must be positive");
  auto base = snapshot(t);
  auto next = base.version + 1;
  return expire_floor(t, base, next > keep ? next - keep + 1 : 1);
}
uint64_t TableStore::expire_older_than(const std::string& t, int64_t timestamp) {
  auto base = snapshot(t);
  auto floor = base.version;
  for (auto& c : history(t))
    if (json_int(c.at("timestamp_ms")) >= timestamp) {
      floor = json_uint(c.at("version"));
      break;
    }
  return expire_floor(t, base, floor);
}
std::vector<Json> TableStore::history(const std::string& t) { return log_.history(t); }
Snapshot TableStore::scan_snapshot(const std::string& t, const ScanOptions& options) {
  if (options.version && options.as_of_ms)
    fail(ErrorCode::InvalidArgument, "choose version or as-of");
  auto s = snapshot(t, options.version);
  if (options.as_of_ms) {
    uint64_t v = 0;
    for (auto& c : history(t))
      if (json_int(c.at("timestamp_ms")) <= *options.as_of_ms) v = json_uint(c.at("version"));
    if (!v) fail(ErrorCode::NotFound, "no snapshot at timestamp");
    s = snapshot(t, v);
  }
  if (options.parallelism == 0 || options.parallelism > 64)
    fail(ErrorCode::InvalidArgument, "parallelism must be 1..64");
  if (options.projection.size() > 1024)
    fail(ErrorCode::InvalidArgument, "projection limited to 1024 columns");
  auto projection = options.projection.empty() ? all_columns(s.schema) : options.projection;
  for (auto c : projection)
    if (c >= s.schema.size()) fail(ErrorCode::InvalidArgument, "projection outside schema");
  for (auto& p : options.predicates) {
    if (p.column >= s.schema.size() || p.literal.index() == 0 ||
        !(p.op == "=" || p.op == "<" || p.op == "<=" || p.op == ">" || p.op == ">="))
      fail(ErrorCode::InvalidArgument, "invalid predicate");
    if (p.literal.index() == 2 && !std::isfinite(std::get<double>(p.literal)))
      fail(ErrorCode::InvalidArgument, "predicate doubles must be finite");
    auto expected = s.schema[p.column].type;
    if (expected == Type::String   ? p.literal.index() != 3
        : expected == Type::Double ? p.literal.index() != 2
                                   : p.literal.index() != 1)
      fail(ErrorCode::InvalidArgument, "predicate literal type mismatch");
  }
  return s;
}

ScanSummary TableStore::scan_stream(const std::string& t, const BatchSink& sink,
                                    const ScanOptions& options, ScanCancellation* external) {
  if (!sink) fail(ErrorCode::InvalidArgument, "scan sink required");
  auto s = scan_snapshot(t, options);
  auto projection = options.projection.empty() ? all_columns(s.schema) : options.projection;
  ScanCancellation internal;
  auto& cancellation = external ? *external : internal;
  ScanBudget budget(options.max_buffer_bytes, cancellation);
  std::mutex sink_mutex;
  std::vector<const DataFile*> files;
  for (const auto& [_, f] : s.files) files.push_back(&f);
  std::vector<ScanMetrics> counters(files.size());
  uint64_t delivered = 0;
  parallel_for(files.size(), options.parallelism, [&](size_t i) {
    try {
      if (cancellation.cancelled()) return;
      if (options.prune && !may_match(files[i]->stats, options.predicates)) {
        counters[i].files_pruned = 1;
        counters[i].bytes_skipped = files[i]->size;
        return;
      }
      stream_file_rows(*store_, files[i]->path, s.schema, options.predicates, projection,
                       counters[i], options.prune, budget, cancellation, [&](const Rows& batch) {
                         std::lock_guard lock(sink_mutex);
                         if (cancellation.cancelled()) return false;
                         delivered += batch.size();
                         try {
                           const bool more = sink(batch);
                           if (!more) cancellation.cancel();
                           return more;
                         } catch (...) {
                           cancellation.cancel();
                           throw;
                         }
                       });
    } catch (...) {
      cancellation.cancel();
      throw;
    }
  });
  ScanSummary out{{}, s.version, delivered, budget.peak(), cancellation.cancelled()};
  for (const auto& c : counters) {
    out.metrics.files_read += c.files_read;
    out.metrics.files_pruned += c.files_pruned;
    out.metrics.groups_pruned += c.groups_pruned;
    out.metrics.data_bytes_read += c.data_bytes_read;
    out.metrics.bytes_skipped += c.bytes_skipped;
  }
  return out;
}
ScanResult TableStore::scan(const std::string& t, const ScanOptions& options) {
  Rows rows;
  auto summary = scan_stream(
      t,
      [&](const Rows& batch) {
        rows.insert(rows.end(), batch.begin(), batch.end());
        return true;
      },
      options);
  return {std::move(rows), summary.metrics, summary.version};
}

void TableStore::verify(const std::string& t) {
  history(t);  // Full replay validates continuity, even if a checkpoint was used by normal reads.
  auto current = snapshot(t);
  std::map<std::string, DataFile> files;
  for (uint64_t v = current.retention_floor; v <= current.version; ++v)
    for (auto& [path, f] : snapshot(t, v).files) files.emplace(path, f);
  for (auto& [path, f] : files) {
    auto m = store_->head(path).value();
    if (m.size != f.size) fail(ErrorCode::Corruption, "file size mismatch");
    ScanMetrics metrics;
    auto rows = read_file_rows(*store_, path, current.schema, {}, all_columns(current.schema),
                               metrics, false);
    if (rows.size() != f.rows) fail(ErrorCode::Corruption, "file row count mismatch");
    const auto footer = read_footer(*store_, path);
    const auto recomputed = encode_file(current.schema, rows).stats;
    if (f.stats != recomputed || footer.stats != recomputed)
      fail(ErrorCode::Corruption, "file statistics mismatch");
    size_t begin = 0;
    for (const auto& group : footer.groups) {
      const auto end = begin + static_cast<size_t>(json_uint(group.at("rows")));
      Rows subset(rows.begin() + static_cast<std::ptrdiff_t>(begin),
                  rows.begin() + static_cast<std::ptrdiff_t>(end));
      if (group.value("stats", Json{}) != encode_file(current.schema, subset).stats)
        fail(ErrorCode::Corruption, "row-group statistics mismatch");
      begin = end;
    }
  }
}
std::map<std::string, DataFile> TableStore::roots() {
  std::set<std::string> tables;
  std::map<std::string, DataFile> live;
  for (auto& obj : store_->list("catalog/").value()) {
    auto j = checked_json(store_->get(obj.key).value());
    auto t = j.at("table").get<std::string>();
    validate_table_id(t);
    if (obj.key != "catalog/" + t + ".json" || j.at("format") != 1)
      fail(ErrorCode::Corruption, "invalid catalog entry");
    tables.insert(t);
  }
  // Fail closed if committed metadata exists without a catalog root.
  for (auto& obj : store_->list("tables/").value()) {
    auto end = obj.key.find('/', 7);
    if (end == std::string::npos) fail(ErrorCode::Corruption, "invalid table object");
    auto t = obj.key.substr(7, end - 7);
    if (obj.key.find("/_log/") != std::string::npos && !tables.contains(t))
      fail(ErrorCode::Corruption, "unregistered table log");
  }
  for (auto& t : tables) {
    if (store_->list("tables/" + t + "/_log/").value().empty())
      continue;  // Registered before publication; unfinished create has no roots.
    auto s = snapshot(t);
    for (uint64_t v = s.retention_floor; v <= s.version; ++v)
      for (auto& [path, f] : snapshot(t, v).files) {
        auto [it, inserted] = live.emplace(path, f);
        if (!inserted && it->second != f)
          fail(ErrorCode::Corruption, "shared file descriptor mismatch");
      }
  }
  for (auto& [path, f] : live) {
    auto meta = store_->head(path);
    if (!meta) throw Failure(meta.error());
    if (meta.value().size != f.size) fail(ErrorCode::Corruption, "reachable file size mismatch");
  }
  return live;
}
GcResult TableStore::gc(std::chrono::milliseconds grace, bool offline, bool dry_run) {
  if (grace.count() < 0 || grace.count() > 365LL * 24 * 60 * 60 * 1000)
    fail(ErrorCode::InvalidArgument, "GC grace must be 0..365 days");
  if (!dry_run && !offline)
    fail(ErrorCode::InvalidArgument,
         "GC deletion requires an offline maintenance window with all namespace clients stopped");
  auto live = roots();
  auto start = now_ms();
  auto manifests = store_->list("gc/pending/").value();
  Json candidates = Json::array();
  GcResult result;
  for (auto& obj : store_->list("tables/").value()) {
    if (obj.key.find("/data/") == std::string::npos || live.contains(obj.key) ||
        obj.meta.modified_ms > start - grace.count())
      continue;
    file_from_json({{"path", obj.key},
                    {"size", obj.meta.size},
                    {"rows", 0}});  // Validate deletion namespace before writing a manifest.
    candidates.push_back({{"path", obj.key}, {"etag", obj.meta.etag}});
    result.paths.push_back(obj.key);
  }
  result.candidates = candidates.size();
  if (dry_run) return result;
  // Process only manifests that existed on entry; a new mark cannot be swept in the same call.
  for (auto& obj : manifests) {
    auto manifest = checked_json(store_->get(obj.key).value());
    if (manifest.at("format") != 1) fail(ErrorCode::Corruption, "unknown GC manifest format");
    auto ready = json_int(manifest.at("ready_after_ms"));
    if (ready > start) continue;
    auto rechecked = roots();
    Json deleted = Json::array();
    for (auto& entry : manifest.at("candidates")) {
      auto path = entry.at("path").get<std::string>();
      validate_key(path);
      if (!valid_data_path(path)) fail(ErrorCode::Corruption, "unsafe GC candidate");
      if (rechecked.contains(path)) continue;
      auto m = store_->head(path);
      if (!m) {
        if (m.error().code == ErrorCode::NotFound) continue;
        throw Failure(m.error());
      }
      if (m.value().etag != entry.at("etag").get<std::string>()) continue;
      store_->erase(path).value();
      ++result.deleted;
      deleted.push_back(path);
    }
    store_
        ->put_if_absent(
            "gc/audit/" + uuid() + ".json",
            Json{{"manifest", obj.key}, {"deleted", deleted}, {"timestamp_ms", now_ms()}}.dump())
        .value();
    store_->erase(obj.key).value();
  }
  if (!candidates.empty())
    store_
        ->put_if_absent("gc/pending/" + uuid() + ".json",
                        Json{{"format", 1},
                             {"ready_after_ms", start + grace.count()},
                             {"candidates", candidates}}
                            .dump())
        .value();
  return result;
}
}  // namespace lakestore
