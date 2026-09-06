#include <barrier>
#include <future>
#include <thread>

#include "lakestore/util/thread.hpp"
#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
class Tables : public ::testing::Test {
 protected:
  std::shared_ptr<MemoryStore> memory = std::make_shared<MemoryStore>();
  std::shared_ptr<FaultInjectingStore> faults = std::make_shared<FaultInjectingStore>(memory);
  TableStore db{faults};
  void SetUp() override { db.create("t", simple_schema()); }
};
TEST_F(Tables, HistoryRestoreAndCloneDivergence) {
  auto v = db.append("t", simple_rows(1));
  db.clone("t", v, "clone");
  db.overwrite("t", simple_rows(2));
  EXPECT_EQ(db.scan("clone").rows, simple_rows(1));
  db.append("clone", simple_rows(3));
  EXPECT_EQ(db.scan("t").rows, simple_rows(2));
  db.restore("t", v);
  EXPECT_EQ(db.scan("t").rows, simple_rows(1));
  ScanOptions old;
  old.version = v;
  EXPECT_EQ(db.scan("t", old).rows, simple_rows(1));
  EXPECT_EQ(db.history("t").size(), 4);
  db.verify("t");
  db.verify("clone");
}
TEST_F(Tables, AsOfAndMonotonicTimestamp) {
  db.append("t", simple_rows(1));
  auto stamp = db.snapshot("t").timestamp_ms;
  db.append("t", simple_rows(2));
  EXPECT_GT(db.snapshot("t").timestamp_ms, stamp);
  ScanOptions at;
  at.as_of_ms = stamp;
  EXPECT_EQ(db.scan("t", at).rows, simple_rows(1));
  at.as_of_ms = 0;
  expect_error(ErrorCode::NotFound, [&] { db.scan("t", at); });
}
TEST_F(Tables, ConcurrentAppendsExactlyOnce) {
  constexpr int writers = 16;
  std::barrier ready(writers);
  std::atomic<int> errors = 0;
  std::vector<JoiningThread> threads;
  for (int i = 0; i < writers; ++i)
    threads.emplace_back([&, i] {
      ready.arrive_and_wait();
      try {
        for (int j = 0; j < 4; ++j) db.append("t", simple_rows(i * 4 + j));
      } catch (...) {
        ++errors;
      }
    });
  threads.clear();
  EXPECT_EQ(errors, 0);
  EXPECT_EQ(db.snapshot("t").version, 65);
  auto rows = db.scan("t").rows;
  std::set<int64_t> ids;
  for (auto& r : rows) ids.insert(std::get<int64_t>(r[0]));
  EXPECT_EQ(rows.size(), 64);
  EXPECT_EQ(ids.size(), 64);
  EXPECT_EQ(db.history("t").size(), 65);
}
TEST_F(Tables, SameVersionRaceRebases) {
  std::barrier ready(2);
  std::atomic<int> hits{0};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 2) && hits.fetch_add(1) < 2)
      ready.arrive_and_wait();
  });
  auto a = std::async(std::launch::async, [&] { return db.append("t", simple_rows(1)); });
  auto b = std::async(std::launch::async, [&] { return db.append("t", simple_rows(2)); });
  a.get();
  b.get();
  EXPECT_EQ(db.scan("t").rows.size(), 2);
  EXPECT_GE(db.log().metrics()->conflicts, 1);
}
TEST_F(Tables, OverwriteAbortsOnConcurrentAppend) {
  db.append("t", simple_rows(0));
  std::promise<void> waiting, release;
  auto future = release.get_future().share();
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 3) && first.exchange(false)) {
      waiting.set_value();
      future.wait();
    }
  });
  auto writer = std::async(std::launch::async, [&] {
    try {
      db.overwrite("t", simple_rows(9));
      return ErrorCode::Fatal;
    } catch (const Failure& e) {
      return e.error().code;
    }
  });
  waiting.get_future().wait();
  db.append("t", simple_rows(1));
  release.set_value();
  EXPECT_EQ(writer.get(), ErrorCode::Conflict);
  EXPECT_EQ(sorted(db.scan("t").rows),
            sorted(Rows{{int64_t{0}, std::string("row")}, {int64_t{1}, std::string("row")}}));
}
TEST_F(Tables, RestoreAbortsOnConcurrentAppend) {
  auto v = db.append("t", simple_rows(1));
  db.overwrite("t", simple_rows(2));
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 4) && first.exchange(false))
      db.append("t", simple_rows(3));
  });
  expect_error(ErrorCode::Conflict, [&] { db.restore("t", v); });
  EXPECT_EQ(db.scan("t").rows.size(), 2);
}
TEST_F(Tables, CompactionPreservesConcurrentAppend) {
  db.append("t", simple_rows(1));
  db.append("t", simple_rows(2));
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 4) && first.exchange(false))
      db.append("t", simple_rows(3));
  });
  db.compact("t");
  EXPECT_EQ(sorted(db.scan("t").rows), sorted(Rows{{int64_t{1}, std::string("row")},
                                                   {int64_t{2}, std::string("row")},
                                                   {int64_t{3}, std::string("row")}}));
}
TEST_F(Tables, CompactionAbortsIfInputRemoved) {
  db.append("t", simple_rows(1));
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 3) && first.exchange(false))
      db.overwrite("t", simple_rows(2));
  });
  expect_error(ErrorCode::Conflict, [&] { db.compact("t"); });
  EXPECT_EQ(db.scan("t").rows, simple_rows(2));
}
TEST_F(Tables, PinnedReaderUnaffectedByOverwrite) {
  auto v = db.append("t", simple_rows(1));
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "get_range" && key.find("/data/") != std::string::npos && first.exchange(false))
      db.overwrite("t", simple_rows(2));
  });
  ScanOptions options;
  options.version = v;
  EXPECT_EQ(db.scan("t", options).rows, simple_rows(1));
  EXPECT_EQ(db.scan("t").rows, simple_rows(2));
}
TEST_F(Tables, TimeoutAfterCommitResolvesWithoutDuplicate) {
  faults->inject({"put_if_absent", log_key("t", 2), ErrorCode::Timeout, true});
  EXPECT_EQ(db.append("t", simple_rows(1)), 2);
  EXPECT_EQ(db.scan("t").rows, simple_rows(1));
  EXPECT_EQ(db.snapshot("t").version, 2);
  EXPECT_EQ(db.log().metrics()->resolutions, 1);
}
TEST_F(Tables, TimeoutBeforeCommitRetriesSameSlot) {
  faults->inject({"put_if_absent", log_key("t", 2), ErrorCode::Timeout, false});
  EXPECT_EQ(db.append("t", simple_rows(1)), 2);
  EXPECT_EQ(db.scan("t").rows.size(), 1);
}
TEST_F(Tables, TimeoutWithOtherWinnerRebases) {
  std::atomic<bool> first{true};
  faults->set_hook([&](auto op, const auto& key) {
    if (op == "put_if_absent" && key == log_key("t", 2) && first.exchange(false)) {
      TableStore other(memory);
      other.append("t", simple_rows(2));
    }
  });
  faults->inject({"put_if_absent", log_key("t", 2), ErrorCode::Timeout});
  db.append("t", simple_rows(1));
  EXPECT_EQ(db.scan("t").rows.size(), 2);
}
TEST_F(Tables, UnresolvedOutcomeIsExplicitAndBounded) {
  for (int i = 0; i < 12; ++i)
    faults->inject({"put_if_absent", log_key("t", 2), ErrorCode::Timeout});
  expect_error(ErrorCode::UnknownOutcome, [&] { db.append("t", simple_rows(1)); });
  EXPECT_TRUE(db.scan("t").rows.empty());
  EXPECT_EQ(db.log().metrics()->resolutions, 12);
}
TEST_F(Tables, FailedResolutionMayStillHaveCommitted) {
  faults->inject({"put_if_absent", log_key("t", 2), ErrorCode::Timeout, true});
  faults->inject({"get", log_key("t", 2), ErrorCode::Fatal});
  expect_error(ErrorCode::UnknownOutcome, [&] { db.append("t", simple_rows(1)); });
  EXPECT_EQ(db.scan("t").rows, simple_rows(1));
}
TEST_F(Tables, CheckpointHintsMissingStaleAndCorrupt) {
  for (int i = 0; i < 22; ++i) db.append("t", simple_rows(i));
  auto expected = sorted(db.scan("t").rows);
  auto hint = memory->get("tables/t/_last_checkpoint").value();
  memory->erase("tables/t/_last_checkpoint").value();
  EXPECT_EQ(sorted(db.scan("t").rows), expected);
  memory->put("tables/t/_last_checkpoint", Json{{"version", 10}}.dump()).value();
  EXPECT_EQ(sorted(db.scan("t").rows), expected);
  memory->put("tables/t/_last_checkpoint", hint).value();
  memory->put("tables/t/_checkpoints/20.json", "bad").value();
  EXPECT_EQ(sorted(db.scan("t").rows), expected);
  ScanOptions old;
  old.version = 2;
  EXPECT_EQ(db.scan("t", old).rows, simple_rows(0));
}
TEST_F(Tables, MissingCommitFailsClosed) {
  db.append("t", simple_rows(1));
  db.append("t", simple_rows(2));
  memory->erase(log_key("t", 2)).value();
  expect_error(ErrorCode::Corruption, [&] { db.scan("t"); });
}
TEST_F(Tables, ExpirationIsDurableAndRejectsAllOldAccess) {
  auto v = db.append("t", simple_rows(1));
  db.overwrite("t", simple_rows(2));
  db.expire_keep_last("t", 1);
  TableStore reopened(memory);
  expect_error(ErrorCode::Expired, [&] { reopened.snapshot("t", v); });
  expect_error(ErrorCode::Expired, [&] { reopened.clone("t", v, "c"); });
  expect_error(ErrorCode::Expired, [&] { reopened.restore("t", v); });
  EXPECT_EQ(reopened.scan("t").rows, simple_rows(2));
}
TEST_F(Tables, FilePruningAndProjection) {
  db.append("t", simple_rows(1));
  db.append("t", simple_rows(100));
  ScanOptions options;
  options.predicates = parse_predicates(simple_schema(), "id < 10");
  options.projection = {1};
  auto pruned = db.scan("t", options);
  options.prune = false;
  auto full = db.scan("t", options);
  EXPECT_EQ(pruned.rows, full.rows);
  EXPECT_EQ(pruned.metrics.files_pruned, 1);
  EXPECT_LT(pruned.metrics.data_bytes_read, full.metrics.data_bytes_read);
}
TEST_F(Tables, RejectInvalidScanOptions) {
  ScanOptions options;
  options.parallelism = 0;
  EXPECT_THROW(db.scan("t", options), Failure);
  options.parallelism = 4;
  options.predicates = {{0, "=", std::string("bad")}};
  EXPECT_THROW(db.scan("t", options), Failure);
  options.predicates = {};
  options.version = 1;
  options.as_of_ms = 1;
  EXPECT_THROW(db.scan("t", options), Failure);
}
TEST_F(Tables, IncompleteCatalogRegistrationCanRecover) {
  db.log().register_table("pending");
  EXPECT_TRUE(db.gc(std::chrono::milliseconds(0), true).paths.empty());
  db.create("pending", simple_schema());
  EXPECT_TRUE(db.scan("pending").rows.empty());
}
TEST_F(Tables, GCRequiresOfflineAndDryRunDoesNotMutate) {
  db.append("t", simple_rows(1));
  db.overwrite("t", simple_rows(2));
  db.expire_keep_last("t", 1);
  expect_error(ErrorCode::InvalidArgument, [&] { db.gc(std::chrono::milliseconds(0), false); });
  auto before = memory->list("").value().size();
  auto r = db.gc(std::chrono::milliseconds(0), false, true);
  EXPECT_EQ(r.candidates, 1);
  EXPECT_EQ(memory->list("").value().size(), before);
}
TEST_F(Tables, GCProtectsCloneAndDeletesAfterBothExpire) {
  auto v = db.append("t", simple_rows(1));
  auto original = db.snapshot("t").files.begin()->first;
  db.clone("t", v, "c");
  db.overwrite("t", simple_rows(2));
  db.expire_keep_last("t", 1);
  db.gc(std::chrono::milliseconds(0), true);
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_TRUE(memory->head(original));
  EXPECT_EQ(db.scan("c").rows, simple_rows(1));
  db.overwrite("c", simple_rows(3));
  db.expire_keep_last("c", 1);
  auto mark = db.gc(std::chrono::milliseconds(0), true);
  EXPECT_GE(mark.candidates, 1);
  EXPECT_TRUE(memory->head(original));
  auto sweep = db.gc(std::chrono::milliseconds(0), true);
  EXPECT_GE(sweep.deleted, 1);
  EXPECT_EQ(memory->head(original).error().code, ErrorCode::NotFound);
  db.verify("t");
  db.verify("c");
}
TEST_F(Tables, GCRechecksRootsBetweenOfflinePasses) {
  auto v = db.append("t", simple_rows(1));
  auto original = db.snapshot("t").files.begin()->first;
  db.overwrite("t", simple_rows(2));
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_TRUE(memory->head(original)); /* Still retained: cannot become a candidate. */
  db.expire_keep_last("t", 1);
  db.gc(std::chrono::milliseconds(0), true);  // candidate now marked
  // Introduce a legitimate reference via a previously existing clone; candidates can be revived
  // only from retained roots.

  EXPECT_THROW(db.clone("t", v, "late"), Failure);
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_FALSE(memory->head(original));
}
TEST_F(Tables, OrphanUploadCollectedInTwoPasses) {
  auto key = "tables/t/data/" + uuid() + ".lsf";
  memory->put(key, encode_file(simple_schema(), simple_rows(9)).bytes).value();
  auto first = db.gc(std::chrono::milliseconds(0), true);
  EXPECT_EQ(first.candidates, 1);
  EXPECT_EQ(first.deleted, 0);
  EXPECT_TRUE(memory->head(key));
  auto second = db.gc(std::chrono::milliseconds(0), true);
  EXPECT_EQ(second.deleted, 1);
  EXPECT_FALSE(memory->head(key));
}
TEST_F(Tables, GracePreventsRecentCandidate) {
  memory
      ->put("tables/t/data/" + uuid() + ".lsf", encode_file(simple_schema(), simple_rows(9)).bytes)
      .value();
  EXPECT_EQ(db.gc(std::chrono::hours(1), true).candidates, 0);
}
TEST_F(Tables, GCFailsClosedOnMissingCatalogOrLiveFile) {
  db.append("t", simple_rows(1));
  memory->erase("catalog/t.json").value();
  expect_error(ErrorCode::Corruption, [&] { db.gc(std::chrono::milliseconds(0), true); });
}
class CrashAfterPut : public StoreDecorator {
 public:
  using StoreDecorator::StoreDecorator;
  Result<ObjectMeta> put_if_absent(const std::string& k, std::string_view bytes) override {
    auto r = inner_->put_if_absent(k, bytes);
    if (k == log_key("t", 2) && r) std::_Exit(17);
    return r;
  }
};
TEST(CrashRecovery, DeathBeforeCommitLeavesOnlyOrphan) {
  TempDir dir;
  auto local = std::make_shared<LocalStore>(dir.path);
  TableStore db(local);
  db.create("t", simple_schema());
  ASSERT_EXIT(
      {
        auto fault = std::make_shared<FaultInjectingStore>(local);
        fault->set_hook([](auto op, const auto& key) {
          if (op == "put_if_absent" && key == log_key("t", 2)) std::_Exit(17);
        });
        TableStore child(fault);
        child.append("t", simple_rows(1));
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(17), "");
  EXPECT_TRUE(db.scan("t").rows.empty());
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_EQ(db.gc(std::chrono::milliseconds(0), true).deleted, 1);
}
TEST(CrashRecovery, DeathAfterCommitLeavesCompleteSnapshot) {
  TempDir dir;
  auto local = std::make_shared<LocalStore>(dir.path);
  TableStore db(local);
  db.create("t", simple_schema());
  ASSERT_EXIT(
      {
        auto crash = std::make_shared<CrashAfterPut>(local);
        TableStore child(crash);
        child.append("t", simple_rows(1));
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(17), "");
  EXPECT_EQ(db.scan("t").rows, simple_rows(1));
  db.verify("t");
}

TEST_F(Tables, HintReadFailureFallsBackToLog) {
  db.append("t", simple_rows(1));
  faults->inject({"get", "tables/t/_last_checkpoint", ErrorCode::Fatal});
  EXPECT_EQ(db.scan("t").rows, simple_rows(1));
}
TEST(PredicateAPI, NonFiniteDoubleLiteralRejected) {
  TableStore db(std::make_shared<MemoryStore>());
  db.create("t", parse_schema("x:double"));
  db.append("t", {{1.0}, {2.0}});
  for (double literal :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    ScanOptions options;
    options.predicates = {{0, "=", literal}};
    expect_error(ErrorCode::InvalidArgument, [&] { db.scan("t", options); });
  }
}
TEST_F(Tables, GCRechecksNewlyPublishedReference) {
  auto encoded = encode_file(simple_schema(), simple_rows(7));
  auto path = "tables/t/data/" + uuid() + ".lsf";
  memory->put_if_absent(path, encoded.bytes).value();
  EXPECT_EQ(db.gc(std::chrono::milliseconds(0), true).candidates, 1);
  auto base = db.snapshot("t");
  DataFile file{path, encoded.bytes.size(), encoded.rows, encoded.stats};
  db.log().commit("t", base, Json::array({Json{{"add", file_json(file)}}}), "append");
  EXPECT_EQ(db.gc(std::chrono::milliseconds(0), true).deleted, 0);
  EXPECT_EQ(db.scan("t").rows, simple_rows(7));
}

TEST_F(Tables, FractionalStoredTimestampFailsClosed) {
  db.append("t", simple_rows(1));
  auto c = Json::parse(memory->get(log_key("t", 2)).value());
  c["timestamp_ms"] = 1.5;
  memory->put(log_key("t", 2), c.dump()).value();
  expect_error(ErrorCode::Corruption, [&] { db.scan("t"); });
}
TEST_F(Tables, VerifyDetectsIncorrectPruningStatistics) {
  db.append("t", simple_rows(1));
  auto c = Json::parse(memory->get(log_key("t", 2)).value());
  c["actions"][0]["add"]["stats"][0]["min"] = 100;
  c["actions"][0]["add"]["stats"][0]["max"] = 100;
  memory->put(log_key("t", 2), c.dump()).value();
  expect_error(ErrorCode::Corruption, [&] { db.verify("t"); });
}

TEST(Metadata, ASCIIIdentifiersAndSafeDataPaths) {
  EXPECT_NO_THROW(validate_table_id("A0_-"));
  EXPECT_NO_THROW(validate_table_id(std::string(64, 'a')));
  for (const auto& id : {std::string{}, std::string(65, 'a'), std::string("_a"), std::string("a/b"),
                         std::string("é")})
    expect_error(ErrorCode::InvalidArgument, [&] { validate_table_id(id); });
  const auto path = "tables/A0_-/data/" + uuid() + ".lsf";
  EXPECT_TRUE(valid_data_path(path));
  for (const auto& invalid :
       {"x/" + path, "tables/_a/data/" + uuid() + ".lsf", path + "/tail",
        "tables/t/data/../" + uuid() + ".lsf", "tables/t/data/" + std::string(36, 'G') + ".lsf"})
    EXPECT_FALSE(valid_data_path(invalid));
}
