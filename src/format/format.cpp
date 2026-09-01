#include "lakestore/format.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
namespace lakestore {
namespace {
constexpr uint64_t kMaxFile = 512ULL * 1024 * 1024, kMaxFooter = 16ULL * 1024 * 1024,
                   kMaxChunk = 64ULL * 1024 * 1024;
std::string type_name(Type t) {
  switch (t) {
    case Type::Int64:
      return "int64";
    case Type::Double:
      return "double";
    case Type::String:
      return "string";
    case Type::Timestamp:
      return "timestamp";
  }
  fail(ErrorCode::Corruption, "invalid type");
}
Type type_from(const std::string& t) {
  if (t == "int64") return Type::Int64;
  if (t == "double") return Type::Double;
  if (t == "string") return Type::String;
  if (t == "timestamp") return Type::Timestamp;
  fail(ErrorCode::InvalidArgument, "unknown column type: " + t);
}
void check_schema(const Schema& s) {
  std::set<std::string> names;
  if (s.empty() || s.size() > 256)
    fail(ErrorCode::InvalidArgument, "schema must have 1..256 columns");
  for (const auto& c : s)
    if (c.name.empty() ||
        !((c.name.front() >= 'A' && c.name.front() <= 'Z') ||
          (c.name.front() >= 'a' && c.name.front() <= 'z') || c.name.front() == '_') ||
        !std::all_of(c.name.begin(), c.name.end(),
                     [](char x) {
                       return (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') ||
                              (x >= '0' && x <= '9') || x == '_';
                     }) ||
        !names.insert(c.name).second)
      fail(ErrorCode::InvalidArgument, "invalid or duplicate column name");
}
void append_u(std::string& out, uint64_t x, unsigned n) {
  for (unsigned i = 0; i < n; ++i) out.push_back(static_cast<char>((x >> (8 * i)) & 255));
}
uint64_t read_u(std::string_view b, size_t& pos, unsigned n) {
  if (pos > b.size() || n > b.size() - pos) fail(ErrorCode::Corruption, "truncated scalar");
  uint64_t x = 0;
  for (unsigned i = 0; i < n; ++i)
    x |= static_cast<uint64_t>(static_cast<unsigned char>(b[pos++])) << (8 * i);
  return x;
}
Json value_json(const Value& v) {
  return std::visit(
      [](const auto& x) -> Json {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return nullptr;
        else
          return x;
      },
      v);
}
int compare(const Value& a, const Value& b) {
  if (a.index() != b.index() || a.index() == 0)
    fail(ErrorCode::InvalidArgument, "incomparable values");
  return std::visit(
      [&](const auto& x) -> int {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return 0;
        else {
          const auto& y = std::get<T>(b);
          return x < y ? -1 : x > y ? 1 : 0;
        }
      },
      a);
}
Value stat_value(const Json& j, const Value& literal) {
  if (literal.index() == 1) {
    if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<uint64_t>() > INT64_MAX))
      fail(ErrorCode::Corruption, "invalid integer statistic");
    return j.get<int64_t>();
  }
  if (literal.index() == 2) {
    if (!j.is_number()) fail(ErrorCode::Corruption, "invalid double statistic");
    auto x = j.get<double>();
    if (!std::isfinite(x)) fail(ErrorCode::Corruption, "non-finite statistic");
    return x;
  }
  if (literal.index() == 3) return j.get<std::string>();
  fail(ErrorCode::Corruption, "invalid statistic");
}
Json stats_for(const Schema& s, const Rows& rows, size_t first, size_t last) {
  Json stats = Json::array();
  for (size_t c = 0; c < s.size(); ++c) {
    Value lo, hi;
    uint64_t nulls = 0;
    for (size_t r = first; r < last; ++r) {
      const auto& v = rows[r][c];
      if (v.index() == 0) {
        ++nulls;
        continue;
      }
      if (lo.index() == 0 || compare(v, lo) < 0) lo = v;
      if (hi.index() == 0 || compare(v, hi) > 0) hi = v;
    }
    stats.push_back({{"min", value_json(lo)}, {"max", value_json(hi)}, {"null_count", nulls}});
  }
  return stats;
}
void validate_value(Type t, const Value& v) {
  if (v.index() == 0) return;
  if ((t == Type::Int64 || t == Type::Timestamp) ? v.index() != 1
      : t == Type::Double                        ? v.index() != 2
                                                 : v.index() != 3)
    fail(ErrorCode::InvalidArgument, "value does not match column type");
  if (v.index() == 2 && !std::isfinite(std::get<double>(v)))
    fail(ErrorCode::InvalidArgument, "non-finite doubles unsupported");
}
std::string chunk(const Rows& rows, size_t col, Type type, size_t first, size_t last) {
  std::string out;
  for (size_t i = first; i < last; ++i) {
    const auto& v = rows[i][col];
    out.push_back(v.index() == 0 ? 0 : 1);
    if (v.index() == 0) continue;
    if (type == Type::String) {
      const auto& x = std::get<std::string>(v);
      if (x.size() > kMaxChunk) fail(ErrorCode::InvalidArgument, "string too large");
      append_u(out, x.size(), 4);
      out += x;
    } else if (type == Type::Double)
      append_u(out, std::bit_cast<uint64_t>(std::get<double>(v)), 8);
    else
      append_u(out, std::bit_cast<uint64_t>(std::get<int64_t>(v)), 8);
    if (out.size() > kMaxChunk) fail(ErrorCode::InvalidArgument, "column chunk exceeds 64 MiB");
  }
  return out;
}
std::vector<Value> decode_chunk(Type t, std::string_view bytes, uint64_t count) {
  std::vector<Value> out;
  out.reserve(static_cast<size_t>(count));
  size_t pos = 0;
  for (uint64_t i = 0; i < count; ++i) {
    auto valid = read_u(bytes, pos, 1);
    if (valid > 1) fail(ErrorCode::Corruption, "invalid null flag");
    if (!valid) {
      out.emplace_back(std::monostate{});
      continue;
    }
    if (t == Type::String) {
      auto n = read_u(bytes, pos, 4);
      if (n > bytes.size() - pos) fail(ErrorCode::Corruption, "string offset outside chunk");
      out.emplace_back(std::string(bytes.substr(pos, static_cast<size_t>(n))));
      pos += static_cast<size_t>(n);
    } else if (t == Type::Double) {
      double x = std::bit_cast<double>(read_u(bytes, pos, 8));
      if (!std::isfinite(x)) fail(ErrorCode::Corruption, "non-finite stored double");
      out.emplace_back(x);
    } else
      out.emplace_back(std::bit_cast<int64_t>(read_u(bytes, pos, 8)));
  }
  if (pos != bytes.size()) fail(ErrorCode::Corruption, "trailing column bytes");
  return out;
}
}  // namespace
uint64_t json_uint(const Json& j) {
  if (!j.is_number_integer() ||
      (j.is_number_integer() && !j.is_number_unsigned() && j.get<int64_t>() < 0))
    fail(ErrorCode::Corruption, "expected nonnegative integer");
  return j.get<uint64_t>();
}
int64_t json_int(const Json& j) {
  if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<uint64_t>() > INT64_MAX))
    fail(ErrorCode::Corruption, "expected signed 64-bit integer");
  return j.get<int64_t>();
}
uint32_t crc32c(std::string_view bytes) {
  // Compile-time Castagnoli table: same on-disk checksum, one lookup per byte.
  static constexpr auto table = [] {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < values.size(); ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
      values[i] = crc;
    }
    return values;
  }();
  uint32_t crc = ~uint32_t{0};
  for (char raw : bytes) crc = (crc >> 8) ^ table[(crc ^ static_cast<unsigned char>(raw)) & 255U];
  return ~crc;
}

Schema parse_schema(const std::string& text) {
  Schema s;
  size_t begin = 0;
  while (begin < text.size()) {
    auto end = text.find(',', begin);
    if (end == std::string::npos) end = text.size();
    auto token = text.substr(begin, end - begin);
    auto colon = token.find(':');
    if (colon == std::string::npos) fail(ErrorCode::InvalidArgument, "schema syntax is name:type");
    s.push_back({token.substr(0, colon), type_from(token.substr(colon + 1))});
    begin = end + 1;
  }
  if (!text.empty() && text.back() == ',')
    fail(ErrorCode::InvalidArgument, "trailing schema separator");
  check_schema(s);
  return s;
}
Json schema_json(const Schema& s) {
  check_schema(s);
  Json j = Json::array();
  for (auto& c : s) j.push_back({{"name", c.name}, {"type", type_name(c.type)}});
  return j;
}
Schema schema_from_json(const Json& j) {
  try {
    if (!j.is_array()) fail(ErrorCode::Corruption, "schema must be an array");
    Schema s;
    for (auto& c : j)
      s.push_back({c.at("name").get<std::string>(), type_from(c.at("type").get<std::string>())});
    check_schema(s);
    return s;
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "invalid stored schema");
  }
}
Value parse_value(Type t, const std::string& text) {
  if (t == Type::String) return text;
  if (t == Type::Double) {
    double x;
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    in >> std::noskipws >> x;
    if (in.fail() || !in.eof() || !std::isfinite(x))
      fail(ErrorCode::InvalidArgument, "invalid finite double");
    return x;
  }
  int64_t x;
  auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), x);
  if (e != std::errc{} || p != text.data() + text.size())
    fail(ErrorCode::InvalidArgument, "invalid int64/timestamp");
  return x;
}
Json row_json(const Row& row) {
  Json out = Json::array();
  for (auto& v : row) out.push_back(value_json(v));
  return out;
}
std::vector<Predicate> parse_predicates(const Schema& s, const std::string& expression) {
  if (expression.empty()) return {};
  // Serialize regex use because some libstdc++ versions lazily mutate shared locale caches.
  static std::mutex parser_mutex;
  std::lock_guard parser_lock(parser_mutex);
  static const std::regex trailing("\\s+AND\\s+$");
  static const std::regex term(
      R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(<=|>=|=|<|>)\s*('(?:[^']|'')*'|[^\s']+)(?:\s+AND\s+|\s*$))");
  std::string rest = expression;
  std::vector<Predicate> out;
  while (!rest.empty()) {
    std::smatch m;
    if (!std::regex_search(rest, m, term))
      fail(ErrorCode::InvalidArgument, "invalid predicate conjunction");
    auto it = std::find_if(s.begin(), s.end(), [&](auto& c) { return c.name == m[1].str(); });
    if (it == s.end()) fail(ErrorCode::InvalidArgument, "unknown predicate column");
    auto literal = m[3].str();
    if (it->type == Type::String) {
      if (literal.size() < 2 || literal.front() != '\'' || literal.back() != '\'')
        fail(ErrorCode::InvalidArgument, "string literals require single quotes");
      literal = literal.substr(1, literal.size() - 2);
      size_t p = 0;
      while ((p = literal.find("''", p)) != std::string::npos) {
        literal.erase(p, 1);
        ++p;
      }
    }
    out.push_back(
        {static_cast<size_t>(it - s.begin()), m[2].str(), parse_value(it->type, literal)});
    bool trailing_and = std::regex_search(m[0].str(), trailing);
    rest = rest.substr(static_cast<size_t>(m.length()));
    if (rest.empty() && trailing_and)
      fail(ErrorCode::InvalidArgument, "missing predicate after AND");
  }
  return out;
}
bool matches(const Row& row, const std::vector<Predicate>& predicates) {
  for (auto& p : predicates) {
    if (p.column >= row.size()) fail(ErrorCode::InvalidArgument, "predicate column outside schema");
    if (row[p.column].index() == 0) return false;
    auto c = compare(row[p.column], p.literal);
    if (p.op == "="    ? c != 0
        : p.op == "<"  ? c >= 0
        : p.op == "<=" ? c > 0
        : p.op == ">"  ? c <= 0
        : p.op == ">=" ? c < 0
                       : true)
      return false;
  }
  return true;
}
bool may_match(const Json& stats, const std::vector<Predicate>& predicates) {
  for (auto& p : predicates) {
    // Missing, malformed or incompatible statistics cannot justify skipping data.
    try {
      if (!stats.is_array() || p.column >= stats.size()) continue;
      const auto& st = stats[p.column];
      if (!st.contains("min") || !st.contains("max")) continue;
      if (st["min"].is_null() || st["max"].is_null()) continue;
      auto lo = stat_value(st["min"], p.literal), hi = stat_value(st["max"], p.literal);
      if (compare(lo, hi) > 0) continue;
      auto l = compare(lo, p.literal), h = compare(hi, p.literal);
      if (p.op == "="    ? (l > 0 || h < 0)
          : p.op == "<"  ? l >= 0
          : p.op == "<=" ? l > 0
          : p.op == ">"  ? h <= 0
          : p.op == ">=" ? h < 0
                         : false)
        return false;
    } catch (const std::exception&) {
      continue;
    }
  }
  return true;
}
EncodedFile encode_file(const Schema& schema, const Rows& rows, size_t group_rows) {
  check_schema(schema);
  if (group_rows == 0 || group_rows > 65536)
    fail(ErrorCode::InvalidArgument, "row group size must be 1..65536");
  for (auto& row : rows) {
    if (row.size() != schema.size()) fail(ErrorCode::InvalidArgument, "row width mismatch");
    for (size_t c = 0; c < row.size(); ++c) validate_value(schema[c].type, row[c]);
  }
  std::string bytes = "LSF1";
  Json groups = Json::array();
  for (size_t begin = 0; begin < rows.size(); begin += group_rows) {
    size_t end = std::min(rows.size(), begin + group_rows);
    Json chunks = Json::array();
    for (size_t c = 0; c < schema.size(); ++c) {
      auto b = chunk(rows, c, schema[c].type, begin, end);
      chunks.push_back({{"offset", bytes.size()}, {"length", b.size()}, {"crc32c", crc32c(b)}});
      bytes += b;
      if (bytes.size() > kMaxFile) fail(ErrorCode::InvalidArgument, "file exceeds 512 MiB");
    }
    groups.push_back({{"rows", end - begin},
                      {"chunks", chunks},
                      {"stats", stats_for(schema, rows, begin, end)}});
  }
  auto stats = stats_for(schema, rows, 0, rows.size());
  auto footer = Json{
      {"version", 1},
      {"schema", schema_json(schema)},
      {"rows", rows.size()},
      {"groups", groups},
      {"stats",
       stats}}.dump();
  if (footer.size() > kMaxFooter || bytes.size() + footer.size() + 16 > kMaxFile)
    fail(ErrorCode::InvalidArgument, "file/footer exceeds size limit");
  bytes += footer;
  append_u(bytes, footer.size(), 8);
  append_u(bytes, crc32c(footer), 4);
  bytes += "LSF1";
  return {std::move(bytes), std::move(stats), rows.size()};
}
FileFooter read_footer(ObjectStore& store, const std::string& key) {
  try {
    auto meta = store.head(key).value();
    if (meta.size < 20 || meta.size > kMaxFile) fail(ErrorCode::Corruption, "invalid LSF size");
    if (store.get_range(key, 0, 4).value() != "LSF1")
      fail(ErrorCode::Corruption, "invalid LSF header");
    auto tail = store.get_range(key, meta.size - 16, 16).value();
    if (tail.size() != 16 || tail.substr(12) != "LSF1")
      fail(ErrorCode::Corruption, "invalid LSF trailer");
    size_t pos = 0;
    auto length = read_u(tail, pos, 8), checksum = read_u(tail, pos, 4);
    if (length > kMaxFooter || length > meta.size - 20)
      fail(ErrorCode::Corruption, "invalid footer length");
    auto start = meta.size - 16 - length;
    auto text = store.get_range(key, start, length).value();
    if (crc32c(text) != checksum) fail(ErrorCode::Corruption, "footer CRC32C mismatch");
    auto j = Json::parse(text);
    if (j.at("version") != 1) fail(ErrorCode::Corruption, "unsupported LSF version");
    FileFooter f{schema_from_json(j.at("schema")), j.at("groups"), j.at("stats"),
                 json_uint(j.at("rows")), start};
    if (!f.groups.is_array()) fail(ErrorCode::Corruption, "invalid row groups");
    uint64_t expected = 4, total = 0;
    for (auto& g : f.groups) {
      auto count = json_uint(g.at("rows"));
      if (count == 0 || count > 65536) fail(ErrorCode::Corruption, "invalid row count");
      total += count;
      const auto& chunks = g.at("chunks");
      if (!chunks.is_array() || chunks.size() != f.schema.size())
        fail(ErrorCode::Corruption, "column chunk count mismatch");
      for (auto& c : chunks) {
        auto off = json_uint(c.at("offset")), n = json_uint(c.at("length"));
        if (off != expected || n > kMaxChunk || off > start || n > start - off || n < count ||
            json_uint(c.at("crc32c")) > UINT32_MAX)
          fail(ErrorCode::Corruption, "invalid column chunk bounds");
        expected += n;
      }
    }
    if (total != f.rows || expected != start)
      fail(ErrorCode::Corruption, "footer row count/data bounds mismatch");
    return f;
  } catch (const Failure&) {
    throw;
  } catch (const std::exception&) {
    fail(ErrorCode::Corruption, "malformed LSF footer");
  }
}
void stream_file_rows(ObjectStore& store, const std::string& key, const Schema& schema,
                      const std::vector<Predicate>& predicates,
                      const std::vector<size_t>& projection, ScanMetrics& metrics, bool prune,
                      ScanBudget& budget, ScanCancellation& cancellation, const BatchSink& sink) {
  if (cancellation.cancelled()) return;
  auto footer = read_footer(store, key);
  if (footer.schema != schema) fail(ErrorCode::Corruption, "file schema mismatch");
  std::set<size_t> needed(projection.begin(), projection.end());
  for (auto& p : predicates) needed.insert(p.column);
  for (auto c : needed)
    if (c >= schema.size()) fail(ErrorCode::InvalidArgument, "column outside schema");
  ++metrics.files_read;
  for (auto& g : footer.groups) {
    if (cancellation.cancelled()) return;
    if (prune && !may_match(g.value("stats", Json{}), predicates)) {
      ++metrics.groups_pruned;
      for (const auto& descriptor : g.at("chunks"))
        metrics.bytes_skipped += json_uint(descriptor.at("length"));
      continue;
    }
    auto count = json_uint(g.at("rows"));
    uint64_t encoded_bytes = 0, max_copies = 1;
    for (auto c : needed) {
      encoded_bytes += json_uint(g.at("chunks")[c].at("length"));
      max_copies = std::max(
          max_copies, static_cast<uint64_t>(std::count(projection.begin(), projection.end(), c)));
    }
    // Raw bytes, decoded string copies, duplicate projections, and row/value storage.
    // Reserve before any data allocation. Metadata and allocator overhead are excluded.
    uint64_t charge = encoded_bytes * 2 * (2 + max_copies) +
                      count * (needed.size() + projection.size()) * sizeof(Value) +
                      count * sizeof(Row) +
                      schema.size() * (sizeof(Value) + sizeof(std::vector<Value>));
    auto reservation = budget.acquire(charge);
    if (!reservation) return;
    Rows output;
    output.reserve(static_cast<size_t>(count));
    std::vector<std::vector<Value>> columns(schema.size());
    for (auto c : needed) {
      if (cancellation.cancelled()) return;
      const auto& desc = g.at("chunks")[c];
      auto n = json_uint(desc.at("length"));
      auto bytes = store.get_range(key, json_uint(desc.at("offset")), n).value();
      metrics.data_bytes_read += bytes.size();
      if (crc32c(bytes) != json_uint(desc.at("crc32c")))
        fail(ErrorCode::Corruption, "column CRC32C mismatch: " + key);
      columns[c] = decode_chunk(schema[c].type, bytes, count);
    }
    for (size_t r = 0; r < count; ++r) {
      if (cancellation.cancelled()) return;
      Row row(schema.size());
      for (auto c : needed) row[c] = std::move(columns[c][r]);
      if (matches(row, predicates)) {
        Row projected;
        projected.reserve(projection.size());
        for (auto c : projection) projected.push_back(row[c]);
        output.push_back(std::move(projected));
      }
    }
    if (!output.empty() && !sink(output)) {
      cancellation.cancel();
      return;
    }
  }
}
Rows read_file_rows(ObjectStore& store, const std::string& key, const Schema& schema,
                    const std::vector<Predicate>& predicates, const std::vector<size_t>& projection,
                    ScanMetrics& metrics, bool prune) {
  Rows output;
  ScanCancellation cancellation;
  ScanBudget budget(1024ULL * 1024 * 1024, cancellation);
  stream_file_rows(store, key, schema, predicates, projection, metrics, prune, budget, cancellation,
                   [&](const Rows& batch) {
                     output.insert(output.end(), batch.begin(), batch.end());
                     return true;
                   });
  return output;
}

Rows read_csv(const Schema& schema, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail(ErrorCode::InvalidArgument, "cannot open CSV");
  std::vector<std::vector<std::string>> records;
  std::vector<std::string> record;
  std::string field;
  bool quoted = false, closed = false, at_start = true;
  char ch;
  while (in.get(ch)) {
    if (quoted) {
      if (ch == '"') {
        if (in.peek() == '"') {
          in.get(ch);
          field += '"';
        } else {
          quoted = false;
          closed = true;
        }
      } else
        field += ch;
      continue;
    }
    if (ch == '"' && at_start) {
      quoted = true;
      at_start = false;
      continue;
    }
    if (ch == ',' || ch == '\n' || ch == '\r') {
      if (ch == '\r' && in.peek() == '\n') in.get(ch);
      record.push_back(std::move(field));
      field.clear();
      closed = false;
      at_start = true;
      if (ch != ',') {
        records.push_back(std::move(record));
        record.clear();
      }
      continue;
    }
    if (closed || ch == '"') fail(ErrorCode::InvalidArgument, "malformed CSV quoting");
    field += ch;
    at_start = false;
  }
  if (in.bad()) fail(ErrorCode::InvalidArgument, "CSV read failed");
  if (quoted) fail(ErrorCode::InvalidArgument, "unterminated CSV quote");
  if (!field.empty() || !record.empty() || !at_start) {
    record.push_back(std::move(field));
    records.push_back(std::move(record));
  }
  if (records.empty() || records[0].size() != schema.size())
    fail(ErrorCode::InvalidArgument, "CSV requires matching header");
  for (size_t i = 0; i < schema.size(); ++i)
    if (records[0][i] != schema[i].name) fail(ErrorCode::InvalidArgument, "CSV header mismatch");
  Rows rows;
  for (size_t r = 1; r < records.size(); ++r) {
    if (records[r].size() != schema.size())
      fail(ErrorCode::InvalidArgument, "CSV row width mismatch");
    Row row;
    for (size_t c = 0; c < schema.size(); ++c) {
      if (records[r][c] == "\\N")
        row.emplace_back(std::monostate{});
      else
        row.push_back(parse_value(schema[c].type, records[r][c]));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}
}  // namespace lakestore
