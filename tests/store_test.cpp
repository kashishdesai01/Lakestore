#include <barrier>
#include <thread>

#include "lakestore/util/thread.hpp"
#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
class StoreConformance : public ::testing::TestWithParam<bool> {
 protected:
  TempDir temp;
  std::shared_ptr<ObjectStore> store;
  void SetUp() override {
    if (GetParam())
      store = std::make_shared<LocalStore>(temp.path);
    else
      store = std::make_shared<MemoryStore>();
  }
};
TEST_P(StoreConformance, MissingAndIdempotentDelete) {
  EXPECT_EQ(store->get("missing").error().code, ErrorCode::NotFound);
  EXPECT_TRUE(store->erase("missing"));
}
TEST_P(StoreConformance, ConditionalWrites) {
  auto first = store->put_if_absent("prefix/object", "abcdef").value();
  EXPECT_EQ(first.size, 6);
  EXPECT_FALSE(first.etag.empty());
  EXPECT_EQ(store->put_if_absent("prefix/object", "bad").error().code, ErrorCode::AlreadyExists);
  EXPECT_EQ(store->put_if_match("prefix/object", "bad", "wrong").error().code,
            ErrorCode::PreconditionFailed);
  auto second = store->put_if_match("prefix/object", "new", first.etag).value();
  EXPECT_NE(first.etag, second.etag);
  EXPECT_EQ(store->get("prefix/object").value(), "new");
  EXPECT_EQ(store->put_if_match("missing", "v", first.etag).error().code,
            ErrorCode::PreconditionFailed);
}
TEST_P(StoreConformance, ExactRangesAndBinaryData) {
  std::string bytes("a\0bc", 4);
  store->put("object", bytes).value();
  EXPECT_EQ(store->get("object").value(), bytes);
  EXPECT_EQ(store->get_range("object", 1, 2).value(), bytes.substr(1, 2));
  EXPECT_EQ(store->get_range("object", 4, 0).value(), "");
  EXPECT_FALSE(store->get_range("object", 3, 2));
  EXPECT_FALSE(store->get_range("object", UINT64_MAX, 1));
}
TEST_P(StoreConformance, CompleteListings) {
  for (int i = 0; i < 1100; ++i) store->put("p/" + std::to_string(i), "x").value();
  store->put("other", "y").value();
  auto list = store->list("p/").value();
  EXPECT_EQ(list.size(), 1100);
  for (auto& item : list) {
    EXPECT_TRUE(item.key.starts_with("p/"));
    EXPECT_EQ(item.meta.size, 1);
    EXPECT_FALSE(item.meta.etag.empty());
  }
}
TEST_P(StoreConformance, OneWinnerUnderContention) {
  std::barrier start(12);
  std::atomic<int> winners{0};
  std::vector<JoiningThread> workers;
  for (int i = 0; i < 12; ++i)
    workers.emplace_back([&, i] {
      start.arrive_and_wait();
      if (store->put_if_absent("race", std::to_string(i))) ++winners;
    });
  workers.clear();
  EXPECT_EQ(winners, 1);
}
TEST_P(StoreConformance, RejectTraversal) {
  for (auto k : {"../escape", "a/../b", "/absolute", "a//b", "a/.hidden", "a/", ""})
    EXPECT_EQ(store->put(k, "bad").error().code, ErrorCode::InvalidArgument);
}
INSTANTIATE_TEST_SUITE_P(Backends, StoreConformance, ::testing::Values(false, true));
TEST(LocalStore, IndependentInstancesAndPersistence) {
  TempDir dir;
  LocalStore a(dir.path), b(dir.path);
  auto m = a.put_if_absent("x", "value").value();
  EXPECT_EQ(b.get("x").value(), "value");
  b.put_if_match("x", "updated", m.etag).value();
  EXPECT_EQ(a.get("x").value(), "updated");
}
TEST(LocalStore, RejectSymlinks) {
  TempDir dir;
  LocalStore s(dir.path);
  std::filesystem::create_directory_symlink(std::filesystem::temp_directory_path(),
                                            dir.path / "objects" / "outside");
  EXPECT_EQ(s.put("outside/file", "bad").error().code, ErrorCode::InvalidArgument);
  EXPECT_FALSE(s.list(""));
}
TEST(Retries, ReadsRetryButConditionalWritesSurfaceTimeout) {
  auto m = std::make_shared<MemoryStore>();
  auto f = std::make_shared<FaultInjectingStore>(m);
  auto metrics = std::make_shared<IoMetrics>();
  RetryingStore r(f, metrics, 4, std::chrono::milliseconds(0));
  m->put("x", "data").value();
  f->inject({"get", "x", ErrorCode::Transient});
  EXPECT_EQ(r.get("x").value(), "data");
  EXPECT_EQ(metrics->retries, 1);
  f->inject({"put_if_absent", "new", ErrorCode::Timeout, true});
  EXPECT_EQ(r.put_if_absent("new", "v").error().code, ErrorCode::Timeout);
  EXPECT_EQ(m->get("new").value(), "v");
  EXPECT_EQ(metrics->retries, 1);
}
TEST(Retries, BoundedAndFatalNotRetried) {
  auto m = std::make_shared<MemoryStore>();
  auto f = std::make_shared<FaultInjectingStore>(m);
  auto metrics = std::make_shared<IoMetrics>();
  RetryingStore r(f, metrics, 2, std::chrono::milliseconds(0));
  for (int i = 0; i < 3; ++i) f->inject({"get", "x", ErrorCode::Timeout});
  EXPECT_EQ(r.get("x").error().code, ErrorCode::Timeout);
  EXPECT_EQ(metrics->requests, 2);
  f->inject({"get", "y", ErrorCode::Fatal});
  EXPECT_EQ(r.get("y").error().code, ErrorCode::Fatal);
  EXPECT_EQ(metrics->requests, 3);
}
