#include <sys/resource.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>

#include "lakestore/table.hpp"
#include "lakestore/util/thread.hpp"
using namespace lakestore;
namespace {
using Clock = std::chrono::steady_clock;
int64_t elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}
uint64_t peak_rss() {
  struct rusage r{};
  getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
  return static_cast<uint64_t>(r.ru_maxrss);
#else
  return static_cast<uint64_t>(r.ru_maxrss) * 1024;
#endif
}
uint64_t number(std::string_view s) {
  uint64_t n;
  auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (e != std::errc{} || p != s.data() + s.size())
    fail(ErrorCode::InvalidArgument, "invalid benchmark number");
  return n;
}
uint32_t crc_reference(std::string_view bytes) {
  uint32_t crc = ~uint32_t{0};
  for (char raw : bytes) {
    crc ^= static_cast<unsigned char>(raw);
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
  }
  return ~crc;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string mode = "scan", layout = "sorted", backend = "memory", path, table = "bench", where;
    uint64_t count = 262144, buffer = 16 * 1024 * 1024;
    size_t parallelism = 4;
    bool materialize = false, prune = true;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--materialize")
        materialize = true;
      else if (arg == "--no-prune")
        prune = false;
      else {
        if (i + 1 >= argc) fail(ErrorCode::InvalidArgument, "missing benchmark option value");
        std::string value = argv[++i];
        if (arg == "--mode")
          mode = value;
        else if (arg == "--layout")
          layout = value;
        else if (arg == "--path") {
          path = value;
          backend = "local-filesystem";
        } else if (arg == "--rows")
          count = number(value);
        else if (arg == "--buffer-mib") {
          auto mib = number(value);
          if (mib == 0 || mib > 1024)
            fail(ErrorCode::InvalidArgument, "buffer MiB must be 1..1024");
          buffer = mib * 1024 * 1024;
        } else if (arg == "--parallelism")
          parallelism = number(value);
        else if (arg == "--where")
          where = value;
        else
          fail(ErrorCode::InvalidArgument, "unknown benchmark option");
      }
    }
    if (parallelism == 0 || parallelism > 64)
      fail(ErrorCode::InvalidArgument, "parallelism must be 1..64");
    Json report{{"mode", mode},
                {"backend", backend},
                {"hardware_threads", std::thread::hardware_concurrency()}};
    if (mode == "crc") {
      std::string bytes(8 * 1024 * 1024, '\0');
      std::mt19937 rng(712);
      for (auto& c : bytes) c = static_cast<char>(rng());
      Json measurements = Json::array();
      for (bool reference : {true, false}) {
        auto start = Clock::now();
        uint32_t checksum = 0;
        for (int i = 0; i < 8; ++i) {
          bytes[0] = static_cast<char>(i);
          checksum = checksum * 16777619U ^ (reference ? crc_reference(bytes) : crc32c(bytes));
        }
        measurements.push_back({{"implementation", reference ? "bitwise-reference" : "production"},
                                {"bytes_processed", bytes.size() * 8},
                                {"microseconds", elapsed(start)},
                                {"checksum", checksum}});
      }
      report["crc"] = measurements;
    } else {
      std::shared_ptr<ObjectStore> store;
      if (path.empty())
        store = std::make_shared<MemoryStore>();
      else
        store = std::make_shared<LocalStore>(path);
      TableStore db(store);
      auto schema = parse_schema("id:int64,payload:string");
      auto prepare = [&] {
        if (count == 0 || count > 10000000)
          fail(ErrorCode::InvalidArgument, "rows must be 1..10000000");
        if (layout != "sorted" && layout != "shuffled" && layout != "skewed")
          fail(ErrorCode::InvalidArgument, "layout must be sorted, shuffled or skewed");
        db.create(table, schema);
        std::mt19937_64 rng(98231);
        // Shuffled is a deterministic bijection, avoiding a full in-memory permutation.
        uint64_t multiplier = 104729;
        while (std::gcd(multiplier, count) != 1) ++multiplier;
        std::string payload(128, 'x');
        for (uint64_t begin = 0; begin < count; begin += 8192) {
          Rows rows;
          rows.reserve(8192);
          for (uint64_t i = begin; i < std::min(count, begin + 8192); ++i) {
            uint64_t id = layout == "sorted"     ? i
                          : layout == "shuffled" ? (i * multiplier + count / 3) % count
                                                 : (rng() % 10 < 9 ? 0 : 1 + rng() % count);
            rows.push_back({static_cast<int64_t>(id), payload});
          }
          db.append(table, rows);
        }
      };
      if (mode == "prepare") {
        auto start = Clock::now();
        prepare();
        report["microseconds"] = elapsed(start);
        report["rows"] = count;
        report["layout"] = layout;
        report["files"] = db.snapshot(table).files.size();
      } else if (mode == "scan") {
        if (path.empty()) prepare();
        auto s = db.snapshot(table);
        ScanOptions options;
        options.parallelism = parallelism;
        options.max_buffer_bytes = buffer;
        options.prune = prune;
        if (where.empty()) where = "id < " + std::to_string(std::max<uint64_t>(1, count / 100));
        options.predicates = parse_predicates(s.schema, where);
        auto before = db.io_metrics()->bytes_read.load();
        auto start = Clock::now();
        uint64_t delivered = 0, peak = 0, checksum = 0;
        ScanMetrics metrics;
        auto consume = [&](const Rows& rows) {
          for (const auto& row : rows) {
            checksum += static_cast<uint64_t>(std::get<int64_t>(row[0]));
            checksum += std::get<std::string>(row[1]).size();
          }
          delivered += rows.size();
          return true;
        };
        if (materialize) {
          auto result = db.scan(table, options);
          consume(result.rows);
          metrics = result.metrics;
        } else {
          auto result = db.scan_stream(table, consume, options);
          metrics = result.metrics;
          peak = result.peak_reserved_bytes;
        }
        report.update({{"layout", layout},
                       {"input_rows", count},
                       {"where", where},
                       {"materialized", materialize},
                       {"prune", prune},
                       {"parallelism", parallelism},
                       {"buffer_budget_bytes", buffer},
                       {"peak_reserved_bytes", peak},
                       {"microseconds", elapsed(start)},
                       {"rows_returned", delivered},
                       {"checksum", checksum},
                       {"bytes_read", db.io_metrics()->bytes_read.load() - before},
                       {"data_bytes_read", metrics.data_bytes_read},
                       {"bytes_skipped", metrics.bytes_skipped},
                       {"files_read", metrics.files_read},
                       {"files_pruned", metrics.files_pruned},
                       {"groups_pruned", metrics.groups_pruned}});
      } else if (mode == "contention") {
        db.create(table, schema);
        std::atomic<unsigned> failures{0};
        auto start = Clock::now();
        std::vector<JoiningThread> writers;
        auto initial = db.log().metrics()->attempts.load();
        for (size_t w = 0; w < parallelism; ++w)
          writers.emplace_back([&, w] {
            try {
              for (int64_t i = 0; i < 20; ++i)
                db.append(table, {{static_cast<int64_t>(w) * 20 + i, std::string("writer")}});
            } catch (...) {
              ++failures;
            }
          });
        writers.clear();
        if (failures) fail(ErrorCode::Fatal, "benchmark writers failed");
        report.update({{"writers", parallelism},
                       {"commits", parallelism * 20},
                       {"microseconds", elapsed(start)},
                       {"attempts", db.log().metrics()->attempts.load() - initial},
                       {"conflicts", db.log().metrics()->conflicts.load()},
                       {"final_rows", db.scan(table).rows.size()}});
      } else if (mode == "startup") {
        prepare();
        Json cases = Json::array();
        for (bool checkpoint : {true, false}) {
          if (!checkpoint) store->erase("tables/bench/_last_checkpoint").value();
          auto before = db.io_metrics()->requests.load();
          auto start = Clock::now();
          auto s = db.snapshot(table);
          cases.push_back({{"checkpoint", checkpoint},
                           {"microseconds", elapsed(start)},
                           {"store_calls", db.io_metrics()->requests.load() - before},
                           {"version", s.version}});
        }
        report["startup"] = cases;
      } else
        fail(ErrorCode::InvalidArgument, "unknown benchmark mode");
    }
    report["peak_rss_bytes"] = peak_rss();
    std::cout << report.dump() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
