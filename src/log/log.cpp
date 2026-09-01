#include "lakestore/log.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
namespace lakestore {
namespace {
bool alnum(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
bool table_id(std::string_view t) {
  return !t.empty() && t.size() <= 64 && alnum(t.front()) &&
         std::all_of(t.begin(), t.end(), [](char c) { return alnum(c) || c == '_' || c == '-'; });
}
}  // namespace
void validate_table_id(const std::string& t) {
  if (!table_id(t))
    fail(ErrorCode::InvalidArgument,
         "table id must have 1..64 alphanumeric, underscore or dash characters");
}
bool valid_data_path(std::string_view path) {
  if (!path.starts_with("tables/")) return false;
  path.remove_prefix(7);
  auto slash = path.find('/');
  if (slash == std::string_view::npos || !table_id(path.substr(0, slash))) return false;
  path.remove_prefix(slash);
  if (!path.starts_with("/data/")) return false;
  path.remove_prefix(6);
  if (path.size() != 40 || !path.ends_with(".lsf")) return false;
  path.remove_suffix(4);
  return std::all_of(path.begin(), path.end(), [](char c) {
    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == '-';
  });
}
std::string log_key(const std::string& t, uint64_t v) {
  validate_table_id(t);
  std::ostringstream s;
  s << "tables/" << t << "/_log/" << std::setfill('0') << std::setw(20) << v << ".json";
  return s.str();
}
Json file_json(const DataFile& f) {
  return {{"path", f.path}, {"size", f.size}, {"rows", f.rows}, {"stats", f.stats}};
}
DataFile file_from_json(const Json& j) {
  try {
    DataFile f{j.at("path").get<std::string>(), json_uint(j.at("size")), json_uint(j.at("rows")),
               j.value("stats", Json{})};
    if (!valid_data_path(f.path) || f.size < 20 || f.size > 512ULL * 1024 * 1024)
      fail(ErrorCode::Corruption, "invalid data descriptor");
    return f;
  } catch (const Failure&) {
    throw;
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "invalid data descriptor");
  }
}
namespace {
Json parse_json(const std::string& text) {
  try {
    return Json::parse(text);
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "malformed metadata JSON");
  }
}
void apply(Snapshot& s, const Json& c, uint64_t expected) {
  try {
    if (json_uint(c.at("version")) != expected || json_uint(c.at("parent")) != s.version ||
        !c.at("id").is_string() || c.at("id").get<std::string>().empty() ||
        !c.at("operation").is_string())
      fail(ErrorCode::Corruption, "commit identity/continuity mismatch");
    auto timestamp = json_int(c.at("timestamp_ms"));
    if (timestamp <= s.timestamp_ms) fail(ErrorCode::Corruption, "non-monotonic commit timestamp");
    if (!c.at("actions").is_array()) fail(ErrorCode::Corruption, "invalid actions");
    for (auto& a : c.at("actions")) {
      if (!a.is_object() || a.size() != 1) fail(ErrorCode::Corruption, "invalid action");
      if (a.contains("metadata")) {
        if (!s.schema.empty()) fail(ErrorCode::Corruption, "schema evolution unsupported");
        s.schema = schema_from_json(a.at("metadata"));
      } else if (a.contains("add")) {
        auto f = file_from_json(a.at("add"));
        if (!s.files.emplace(f.path, f).second) fail(ErrorCode::Corruption, "duplicate live file");
      } else if (a.contains("remove")) {
        auto path = a.at("remove").get<std::string>();
        if (s.files.erase(path) != 1) fail(ErrorCode::Corruption, "removing non-live file");
      } else if (a.contains("expire_before")) {
        auto floor = json_uint(a.at("expire_before"));
        if (floor < s.retention_floor || floor > expected || floor == 0)
          fail(ErrorCode::Corruption, "invalid retention floor");
        s.retention_floor = floor;
      } else if (a.contains("clone_source")) {
        validate_table_id(a.at("clone_source").at("table").get<std::string>());
        if (json_uint(a.at("clone_source").at("version")) == 0)
          fail(ErrorCode::Corruption, "invalid clone version");
      } else
        fail(ErrorCode::Corruption, "unknown log action");
    }
    if (s.schema.empty()) fail(ErrorCode::Corruption, "missing schema");
    s.version = expected;
    s.timestamp_ms = timestamp;
  } catch (const Failure&) {
    throw;
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "malformed commit");
  }
}
Json snapshot_json(const Snapshot& s) {
  Json files = Json::array();
  for (auto& [_, f] : s.files) files.push_back(file_json(f));
  return {{"version", s.version},
          {"timestamp_ms", s.timestamp_ms},
          {"schema", schema_json(s.schema)},
          {"files", files},
          {"retention_floor", s.retention_floor}};
}
Snapshot snapshot_from(const Json& j) {
  Snapshot s;
  s.version = json_uint(j.at("version"));
  s.timestamp_ms = json_int(j.at("timestamp_ms"));
  s.schema = schema_from_json(j.at("schema"));
  s.retention_floor = json_uint(j.at("retention_floor"));
  if (!s.version || s.timestamp_ms <= 0 || !s.retention_floor || s.retention_floor > s.version)
    fail(ErrorCode::Corruption, "invalid checkpoint state");
  for (auto& d : j.at("files")) {
    auto f = file_from_json(d);
    if (!s.files.emplace(f.path, f).second)
      fail(ErrorCode::Corruption, "duplicate checkpoint file");
  }
  return s;
}
bool uncertain(ErrorCode c) { return c == ErrorCode::Timeout || c == ErrorCode::Transient; }
}  // namespace
uint64_t Log::latest_version(const std::string& t) {
  validate_table_id(t);
  auto prefix = "tables/" + t + "/_log/";
  auto objects = store_->list(prefix).value();
  uint64_t latest = 0;
  for (auto& obj : objects) {
    auto name = obj.key.substr(prefix.size());
    if (name.size() != 25 || name.substr(20) != ".json")
      fail(ErrorCode::Corruption, "unexpected object in commit log");
    uint64_t v;
    auto [p, e] = std::from_chars(name.data(), name.data() + 20, v);
    if (e != std::errc{} || p != name.data() + 20 || v == 0 || v > static_cast<uint64_t>(INT64_MAX))
      fail(ErrorCode::Corruption, "invalid commit version");
    latest = std::max(latest, v);
  }
  return latest;
}
Snapshot Log::reconstruct(const std::string& t, uint64_t version) {
  Snapshot s;
  // Hint is optional and untrusted for availability: fall back to complete replay on any bad hint.
  auto hint = store_->get("tables/" + t + "/_last_checkpoint");
  if (hint) {
    try {
      auto v = json_uint(parse_json(hint.value()).at("version"));
      if (v > 0 && v <= version) {
        auto envelope = parse_json(
            store_->get("tables/" + t + "/_checkpoints/" + std::to_string(v) + ".json").value());
        auto text = envelope.at("state").get<std::string>();
        if (json_uint(envelope.at("crc32c")) != crc32c(text))
          fail(ErrorCode::Corruption, "checkpoint checksum mismatch");
        auto candidate = snapshot_from(parse_json(text));
        if (candidate.version != v) fail(ErrorCode::Corruption, "checkpoint version mismatch");
        auto commit = parse_json(store_->get(log_key(t, v)).value());
        if (json_uint(commit.at("version")) != v ||
            json_int(commit.at("timestamp_ms")) != candidate.timestamp_ms)
          fail(ErrorCode::Corruption, "checkpoint commit mismatch");
        s = std::move(candidate);
      }
    } catch (const std::exception&) {
      s = Snapshot{};
    }
  }  // Hint read failures also fall back to replay; it is never authoritative.
  for (uint64_t v = s.version + 1; v <= version; ++v) {
    auto c = store_->get(log_key(t, v));
    if (!c && c.error().code == ErrorCode::NotFound)
      fail(ErrorCode::Corruption, "gap in commit log at " + std::to_string(v));
    apply(s, parse_json(c.value()), v);
  }
  return s;
}
Snapshot Log::latest(const std::string& t) {
  auto version = latest_version(t);
  if (!version) fail(ErrorCode::NotFound, "table has no committed snapshot: " + t);
  return reconstruct(t, version);
}
Snapshot Log::at(const std::string& t, uint64_t v) {
  auto current = latest(t);
  if (v < current.retention_floor) fail(ErrorCode::Expired, "snapshot expired");
  if (v > current.version || v == 0) fail(ErrorCode::NotFound, "snapshot not found");
  return v == current.version ? current : reconstruct(t, v);
}
std::vector<Json> Log::history(const std::string& t) {
  auto current = latest(t);
  std::vector<Json> out;
  Snapshot state;
  for (uint64_t v = 1; v <= current.version; ++v) {
    auto j = parse_json(store_->get(log_key(t, v)).value());
    apply(state, j, v);
    j["retained"] = v >= current.retention_floor;
    out.push_back(std::move(j));
  }
  return out;
}
void Log::register_table(const std::string& t) {
  validate_table_id(t);
  auto key = "catalog/" + t + ".json";
  auto payload = Json{{"table", t}, {"format", 1}}.dump();
  for (unsigned i = 0; i < 8; ++i) {
    auto r = store_->put_if_absent(key, payload);
    if (r) return;
    if (r.error().code == ErrorCode::AlreadyExists ||
        r.error().code == ErrorCode::PreconditionFailed || uncertain(r.error().code)) {
      auto existing = store_->get(key);
      if (existing) {
        if (existing.value() != payload)
          fail(ErrorCode::Corruption, "catalog registration mismatch");
        return;
      }
      if (existing.error().code != ErrorCode::NotFound && !uncertain(existing.error().code))
        throw Failure(existing.error());
    } else
      throw Failure(r.error());
  }
  fail(ErrorCode::UnknownOutcome, "catalog registration outcome unknown: " + t);
}
uint64_t Log::commit(const std::string& t, const Snapshot& base, Json actions,
                     const std::string& operation, bool strict,
                     const std::vector<std::string>& removals) {
  validate_table_id(t);
  Snapshot parent = base;
  auto id = uuid();
  for (unsigned rebase = 0; rebase < 256; ++rebase) {
    if (parent.version >= static_cast<uint64_t>(INT64_MAX) || parent.timestamp_ms == INT64_MAX)
      fail(ErrorCode::Fatal, "version/timestamp overflow");
    auto v = parent.version + 1;
    auto key = log_key(t, v);
    Json c{{"version", v},
           {"parent", parent.version},
           {"id", id},
           {"timestamp_ms", std::max(now_ms(), parent.timestamp_ms + 1)},
           {"operation", operation},
           {"actions", actions}};
    Snapshot candidate = parent;
    apply(candidate, c, v);
    auto bytes = c.dump();
    bool other_won = false;
    for (unsigned resolve = 0; resolve < 12; ++resolve) {
      ++metrics_->attempts;
      auto result = store_->put_if_absent(key, bytes);
      if (result) {
        if (checkpoint_every_ && v % checkpoint_every_ == 0) {
          try {
            checkpoint(t, candidate);
          } catch (const std::exception&) { /* Commit already durable. Checkpoints are optional. */
          }
        }
        return v;
      }
      auto code = result.error().code;
      if (code != ErrorCode::AlreadyExists && code != ErrorCode::PreconditionFailed &&
          !uncertain(code))
        throw Failure(result.error());
      if (uncertain(code)) ++metrics_->resolutions;
      auto existing = store_->get(key);
      if (existing) {
        auto winner = parse_json(existing.value());
        if (winner.at("id").get<std::string>() == id) {
          if (existing.value() != bytes) fail(ErrorCode::Corruption, "commit id payload mismatch");
          return v;
        }
        other_won = true;
        ++metrics_->conflicts;
        break;
      }
      if (existing.error().code != ErrorCode::NotFound && !uncertain(existing.error().code))
        fail(ErrorCode::UnknownOutcome, "cannot resolve commit id=" + id + " key=" + key);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::min(50U, 1U << std::min(resolve, 5U))));
    }
    if (!other_won)
      fail(ErrorCode::UnknownOutcome, "commit outcome unknown id=" + id + " key=" + key);
    if (strict) fail(ErrorCode::Conflict, "table changed during " + operation);
    parent = latest(t);
    for (auto& path : removals)
      if (!parent.files.contains(path))
        fail(ErrorCode::Conflict, "rewritten file was concurrently removed");
  }
  fail(ErrorCode::Conflict, "commit contention exceeded retry budget");
}
void Log::checkpoint(const std::string& t, const Snapshot& s) {
  auto text = snapshot_json(s).dump();
  auto key = "tables/" + t + "/_checkpoints/" + std::to_string(s.version) + ".json";
  auto r = store_->put_if_absent(key, Json{{"state", text}, {"crc32c", crc32c(text)}}.dump());
  if (!r && r.error().code != ErrorCode::AlreadyExists) throw Failure(r.error());
  store_->put("tables/" + t + "/_last_checkpoint", Json{{"version", s.version}}.dump()).value();
}
}  // namespace lakestore
