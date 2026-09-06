#include <condition_variable>
#include <future>
#include <thread>

#include "lakestore/util/thread.hpp"
#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
class Streaming : public ::testing::Test {
 protected:
  std::shared_ptr<MemoryStore> memory = std::make_shared<MemoryStore>();
  std::shared_ptr<FaultInjectingStore> faults = std::make_shared<FaultInjectingStore>(memory);
  TableStore db{faults};
  void SetUp() override {
    db.create("t", parse_schema("id:int64,label:string"));
    Rows rows;
    for (int64_t i = 0; i < 4096; ++i) rows.push_back({i, std::string(64, 'a')});
    for (int i = 0; i < 4; ++i) db.append("t", rows);
  }
};
TEST_F(Streaming, EqualsMaterializedWithPruningAndDuplicateProjection) {
  ScanOptions options;
  options.predicates = parse_predicates(simple_schema(), "id > 1000 AND id < 2000");
  options.projection = {1, 1, 0};
  options.max_buffer_bytes = 2 * 1024 * 1024;
  Rows collected;
  auto summary = db.scan_stream(
      "t",
      [&](const Rows& batch) {
        collected.insert(collected.end(), batch.begin(), batch.end());
        return true;
      },
      options);
  EXPECT_EQ(sorted(collected), sorted(db.scan("t", options).rows));
  EXPECT_EQ(summary.rows_delivered, collected.size());
  EXPECT_FALSE(summary.cancelled);
  EXPECT_GT(summary.peak_reserved_bytes, 0);
  EXPECT_LE(summary.peak_reserved_bytes, options.max_buffer_bytes);
}
TEST_F(Streaming, EarlyConsumerTerminationStopsAfterOneBatchAndJoins) {
  ScanOptions options;
  options.max_buffer_bytes = 2 * 1024 * 1024;
  std::atomic<int> callbacks{0};
  auto summary = db.scan_stream(
      "t",
      [&](const Rows&) {
        ++callbacks;
        return false;
      },
      options);
  EXPECT_EQ(callbacks, 1);
  EXPECT_EQ(summary.rows_delivered, 1024);
  EXPECT_TRUE(summary.cancelled);
  EXPECT_LE(summary.peak_reserved_bytes, options.max_buffer_bytes);
}
TEST_F(Streaming, ConsumerExceptionPropagatesAndJoinsBlockedWorkers) {
  ScanOptions options;
  options.max_buffer_bytes = 1024 * 1024;
  EXPECT_THROW(
      db.scan_stream(
          "t", [](const Rows&) -> bool { throw std::runtime_error("consumer failed"); }, options),
      std::runtime_error);
  EXPECT_EQ(db.scan("t").rows.size(), 16384);
}
TEST_F(Streaming, PreCancelledDoesNotFetchData) {
  ScanCancellation cancellation;
  cancellation.cancel();
  std::atomic<int> reads{0};
  faults->set_hook([&](auto op, const auto&) {
    if (op == "get_range") ++reads;
  });
  auto summary = db.scan_stream("t", [](const Rows&) { return true; }, {}, &cancellation);
  EXPECT_TRUE(summary.cancelled);
  EXPECT_EQ(summary.rows_delivered, 0);
  EXPECT_EQ(reads, 0);
}
TEST_F(Streaming, ExternalCancellationDuringSlowConsumer) {
  ScanCancellation cancellation;
  std::promise<void> entered, resume;
  auto released = resume.get_future().share();
  std::atomic<int> callbacks{0};
  auto scan = std::async(std::launch::async, [&] {
    ScanOptions options;
    options.max_buffer_bytes = 1024 * 1024;
    return db.scan_stream(
        "t",
        [&](const Rows&) {
          ++callbacks;
          entered.set_value();
          released.wait();
          return true;
        },
        options, &cancellation);
  });
  entered.get_future().wait();
  cancellation.cancel();
  resume.set_value();
  auto summary = scan.get();
  EXPECT_EQ(callbacks, 1);
  EXPECT_TRUE(summary.cancelled);
}
TEST_F(Streaming, BackpressureSerializesConsumerCallbacks) {
  std::atomic<int> active{0}, maximum{0};
  auto summary = db.scan_stream("t", [&](const Rows&) {
    int n = ++active;
    maximum = std::max(maximum.load(), n);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    --active;
    return true;
  });
  EXPECT_EQ(maximum, 1);
  EXPECT_EQ(summary.rows_delivered, 16384);
}
TEST_F(Streaming, TooSmallBudgetFailsInsteadOfDeadlocking) {
  ScanOptions options;
  options.max_buffer_bytes = 1;
  expect_error(ErrorCode::InvalidArgument,
               [&] { db.scan_stream("t", [](const Rows&) { return true; }, options); });
  options.max_buffer_bytes = 0;
  expect_error(ErrorCode::InvalidArgument,
               [&] { db.scan_stream("t", [](const Rows&) { return true; }, options); });
}
TEST_F(Streaming, CorruptionStopsAllWorkersAndThrows) {
  auto path = db.snapshot("t").files.begin()->first;
  auto bytes = memory->get(path).value();
  bytes[5] ^= 1;
  memory->put(path, bytes).value();
  expect_error(ErrorCode::Corruption,
               [&] { db.scan_stream("t", [](const Rows&) { return true; }); });
}
TEST_F(Streaming, SnapshotRemainsPinnedAcrossConsumerWrites) {
  auto version = db.snapshot("t").version;
  std::atomic<bool> once{true};
  auto summary = db.scan_stream("t", [&](const Rows&) {
    if (once.exchange(false)) db.overwrite("t", simple_rows(-1));
    return true;
  });
  EXPECT_EQ(summary.version, version);
  EXPECT_EQ(summary.rows_delivered, 16384);
  EXPECT_EQ(db.scan("t").rows, simple_rows(-1));
}
