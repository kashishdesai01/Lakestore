#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

#include "lakestore/table.hpp"
using namespace lakestore;
namespace {
std::string env(const char* name) {
  auto value = std::getenv(name);
  return value ? value : "";
}
uint64_t number(const std::string& text) {
  uint64_t value = 0;
  auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (e != std::errc{} || p != text.data() + text.size())
    fail(ErrorCode::InvalidArgument, "expected nonnegative integer");
  return value;
}
std::chrono::milliseconds duration(const std::string& text) {
  auto pos = text.find_first_not_of("0123456789");
  if (pos == 0 || pos == std::string::npos)
    fail(ErrorCode::InvalidArgument, "duration requires ms,s,m,h,d suffix");
  auto unit = text.substr(pos);
  uint64_t multiplier = unit == "ms"  ? 1
                        : unit == "s" ? 1000
                        : unit == "m" ? 60000
                        : unit == "h" ? 3600000
                        : unit == "d" ? 86400000
                                      : 0;
  auto n = number(text.substr(0, pos));
  if (!multiplier || n > static_cast<uint64_t>(INT64_MAX) / multiplier)
    fail(ErrorCode::InvalidArgument, "invalid duration");
  return std::chrono::milliseconds(n * multiplier);
}
#ifdef LAKESTORE_CLOUD
CloudConfig cloud_config(const std::string& uri) {
  CloudConfig c;
  c.provider = uri.starts_with("s3://") ? CloudConfig::Provider::S3 : CloudConfig::Provider::Azure;
  auto start = uri.find("://") + 3;
  auto slash = uri.find('/', start);
  c.container = uri.substr(start, slash == std::string::npos ? slash : slash - start);
  if (slash != std::string::npos) c.prefix = uri.substr(slash + 1);
  if (c.provider == CloudConfig::Provider::S3) {
    c.region = env("AWS_REGION");
    if (c.region.empty()) c.region = "us-east-1";
    c.endpoint = env("LAKESTORE_S3_ENDPOINT");
    if (c.endpoint.empty()) c.endpoint = "https://s3." + c.region + ".amazonaws.com";
    c.access_key = env("AWS_ACCESS_KEY_ID");
    c.secret_key = env("AWS_SECRET_ACCESS_KEY");
    c.session_token = env("AWS_SESSION_TOKEN");
  } else {
    c.account = env("AZURE_STORAGE_ACCOUNT");
    c.account_key = env("AZURE_STORAGE_KEY");
    c.bearer_token = env("AZURE_STORAGE_BEARER_TOKEN");
    c.endpoint = env("LAKESTORE_AZURE_ENDPOINT");
    if (c.endpoint.empty()) c.endpoint = "https://" + c.account + ".blob.core.windows.net";
  }
  return c;
}
#endif
void help() {
  std::cout << R"(lakestore COMMAND --store URI [options]
URI: file:///absolute/path | s3://bucket/prefix | azure://container/prefix
Commands:
  init-store                         provision a cloud bucket/container
  create --table T --schema name:type,...
  append|overwrite --table T --input data.csv
  scan --table T [--where EXPR] [--version N|--as-of milliseconds]
       [--columns name,...] [--parallelism N] [--no-prune]
       [--stream] [--buffer-mib N]  # JSONL rows and final scan summary
  history|verify|compact --table T
  restore --table T --version N
  clone --source T --version N --as TARGET
  expire-snapshots --table T --keep-last K|--older-than milliseconds
  gc [--grace 1h] [--dry-run|--offline]
GC: stop ALL clients sharing the namespace before --offline; run twice,
separated by the grace duration. Online GC is unsupported.
CSV requires matching header; null is \N; timestamps are epoch milliseconds.
Output: JSON on stdout; metrics/errors on stderr. LAKESTORE_STORE supplies URI.
)";
}
}  // namespace
int main(int argc, char** argv) {
  // A closed downstream pipe must fail the scan rather than terminate workers with SIGPIPE.
  std::signal(SIGPIPE, SIG_IGN);
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
      help();
      return 0;
    }
    std::string command = argv[1];
    std::map<std::string, std::string> args;
    std::set<std::string> flags{"--offline", "--dry-run", "--no-prune", "--stream"};
    for (int i = 2; i < argc; ++i) {
      std::string name = argv[i];
      if (!name.starts_with("--") || args.contains(name))
        fail(ErrorCode::InvalidArgument, "invalid or duplicate option");
      if (flags.contains(name))
        args[name] = "true";
      else {
        if (i + 1 >= argc || std::string(argv[i + 1]).starts_with("--"))
          fail(ErrorCode::InvalidArgument, "missing option value");
        args[name] = argv[++i];
      }
    }
    std::map<std::string, std::set<std::string>> allowed{
        {"init-store", {}},
        {"create", {"--table", "--schema"}},
        {"append", {"--table", "--input"}},
        {"overwrite", {"--table", "--input"}},
        {"scan",
         {"--table", "--where", "--version", "--as-of", "--columns", "--parallelism", "--no-prune",
          "--stream", "--buffer-mib"}},
        {"history", {"--table"}},
        {"verify", {"--table"}},
        {"compact", {"--table"}},
        {"restore", {"--table", "--version"}},
        {"clone", {"--source", "--version", "--as"}},
        {"expire-snapshots", {"--table", "--keep-last", "--older-than"}},
        {"gc", {"--grace", "--offline", "--dry-run"}}};
    if (!allowed.contains(command)) fail(ErrorCode::InvalidArgument, "unknown command");
    for (auto& [name, _] : args)
      if (name != "--store" && !allowed[command].contains(name))
        fail(ErrorCode::InvalidArgument, "option unsupported by command: " + name);
    auto require = [&](const std::string& name) {
      if (!args.contains(name) || args.at(name).empty())
        fail(ErrorCode::InvalidArgument, "required option: " + name);
      return args.at(name);
    };
    auto uri = args.contains("--store") ? args.at("--store") : env("LAKESTORE_STORE");
    if (uri.empty()) fail(ErrorCode::InvalidArgument, "--store or LAKESTORE_STORE required");
    std::shared_ptr<ObjectStore> store;
    if (uri.starts_with("file://")) {
      auto path = uri.substr(7);
      if (path.empty() || path[0] != '/')
        fail(ErrorCode::InvalidArgument, "file store requires an absolute path");
      store = std::make_shared<LocalStore>(path);
    }
#ifdef LAKESTORE_CLOUD
    else if (uri.starts_with("s3://") || uri.starts_with("azure://")) {
      auto c = cloud_config(uri);
      if (command == "init-store") {
        initialize_cloud(c).value();
        std::cout << Json{{"initialized", true}}.dump() << '\n';
        return 0;
      }
      store = cloud_store(c);
    }
#endif
    else
      fail(ErrorCode::InvalidArgument, "unsupported store URI or cloud support disabled");
    TableStore tables(store);
    Json output;
    auto started = now_ms();
    uint64_t version = 0;
    bool output_written = false;
    if (command == "init-store")
      output = {{"initialized", true}};
    else if (command == "create")
      version = tables.create(require("--table"), parse_schema(require("--schema")));
    else if (command == "append" || command == "overwrite") {
      auto t = require("--table");
      auto s = tables.snapshot(t);
      auto rows = read_csv(s.schema, require("--input"));
      version = command == "append" ? tables.append(t, rows) : tables.overwrite(t, rows);
    } else if (command == "restore")
      version = tables.restore(require("--table"), number(require("--version")));
    else if (command == "clone")
      version = tables.clone(require("--source"), number(require("--version")), require("--as"));
    else if (command == "compact")
      version = tables.compact(require("--table"));
    else if (command == "expire-snapshots") {
      auto t = require("--table");
      if (args.contains("--keep-last") == args.contains("--older-than"))
        fail(ErrorCode::InvalidArgument, "choose exactly one expiration policy");
      if (args.contains("--keep-last"))
        version = tables.expire_keep_last(t, number(args.at("--keep-last")));
      else {
        auto n = number(args.at("--older-than"));
        if (n > INT64_MAX) fail(ErrorCode::InvalidArgument, "timestamp overflow");
        version = tables.expire_older_than(t, static_cast<int64_t>(n));
      }
    } else if (command == "scan") {
      auto t = require("--table");
      auto s = tables.snapshot(t);
      ScanOptions options;
      if (args.contains("--where"))
        options.predicates = parse_predicates(s.schema, args.at("--where"));
      if (args.contains("--version")) options.version = number(args.at("--version"));
      if (args.contains("--as-of")) {
        auto n = number(args.at("--as-of"));
        if (n > INT64_MAX) fail(ErrorCode::InvalidArgument, "timestamp overflow");
        options.as_of_ms = static_cast<int64_t>(n);
      }
      if (args.contains("--parallelism")) options.parallelism = number(args.at("--parallelism"));
      options.prune = !args.contains("--no-prune");
      if (args.contains("--buffer-mib")) {
        auto mib = number(args.at("--buffer-mib"));
        if (!mib || mib > 1024) fail(ErrorCode::InvalidArgument, "buffer-mib must be 1..1024");
        options.max_buffer_bytes = mib * 1024 * 1024;
      }
      if (args.contains("--columns")) {
        auto text = require("--columns");
        size_t begin = 0;
        do {
          auto end = text.find(',', begin);
          auto name = text.substr(begin, end == std::string::npos ? end : end - begin);
          auto it = std::find_if(s.schema.begin(), s.schema.end(),
                                 [&](auto& c) { return c.name == name; });
          if (it == s.schema.end()) fail(ErrorCode::InvalidArgument, "unknown projection column");
          options.projection.push_back(static_cast<size_t>(it - s.schema.begin()));
          if (end == std::string::npos) break;
          begin = end + 1;
        } while (begin <= text.size());
      }
      if (args.contains("--stream")) {
        auto summary = tables.scan_stream(
            t,
            [&](const Rows& batch) {
              for (const auto& row : batch) std::cout << row_json(row).dump() << '\n';
              std::cout.flush();
              if (!std::cout) fail(ErrorCode::Fatal, "scan output pipe closed or write failed");
              return true;
            },
            options);
        std::cout << Json{{"event", "scan_complete"},
                          {"version", summary.version},
                          {"rows", summary.rows_delivered},
                          {"cancelled", summary.cancelled},
                          {"peak_reserved_bytes", summary.peak_reserved_bytes},
                          {"files_read", summary.metrics.files_read},
                          {"files_pruned", summary.metrics.files_pruned},
                          {"data_bytes_read", summary.metrics.data_bytes_read}}
                         .dump()
                  << '\n';
        std::cout.flush();
        if (!std::cout) fail(ErrorCode::Fatal, "scan summary output failed");
        output_written = true;
      } else {
        auto result = tables.scan(t, options);
        output = {{"version", result.version},
                  {"rows", Json::array()},
                  {"scan",
                   {{"files_read", result.metrics.files_read},
                    {"files_pruned", result.metrics.files_pruned},
                    {"groups_pruned", result.metrics.groups_pruned},
                    {"data_bytes_read", result.metrics.data_bytes_read},
                    {"bytes_skipped", result.metrics.bytes_skipped}}}};
        for (auto& row : result.rows) output["rows"].push_back(row_json(row));
      }
    } else if (command == "history") {
      output = Json::array();
      for (auto& c : tables.history(require("--table")))
        output.push_back({{"version", c["version"]},
                          {"timestamp_ms", c["timestamp_ms"]},
                          {"id", c["id"]},
                          {"operation", c["operation"]},
                          {"retained", c["retained"]}});
    } else if (command == "verify") {
      tables.verify(require("--table"));
      output = {{"verified", true}};
    } else if (command == "gc") {
      auto result =
          tables.gc(args.contains("--grace") ? duration(args.at("--grace")) : std::chrono::hours(1),
                    args.contains("--offline"), args.contains("--dry-run"));
      output = {{"candidates", result.candidates},
                {"deleted", result.deleted},
                {"paths", result.paths},
                {"dry_run", args.contains("--dry-run")}};
    }
    if (version) output = {{"version", version}};
    if (!output_written) std::cout << output.dump() << '\n';
    auto io = tables.io_metrics();
    auto txn = tables.log().metrics();
    std::cerr << Json{{"event", "command_complete"},
                      {"command", command},
                      {"elapsed_ms", now_ms() - started},
                      {"requests", io->requests.load()},
                      {"bytes_read", io->bytes_read.load()},
                      {"io_retries", io->retries.load()},
                      {"commit_attempts", txn->attempts.load()},
                      {"commit_conflicts", txn->conflicts.load()},
                      {"ambiguous_resolutions", txn->resolutions.load()}}
                     .dump()
              << '\n';
    return 0;
  } catch (const Failure& e) {
    std::cerr << Json{{"error", static_cast<int>(e.error().code)}, {"message", e.what()}}.dump()
              << '\n';
    return e.error().code == ErrorCode::UnknownOutcome ? 3 : 1;
  } catch (const std::exception&) {
    std::cerr << "{\"error\":\"unexpected failure\"}\n";
    return 1;
  }
}
