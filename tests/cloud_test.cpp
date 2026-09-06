#include <cstdlib>
#include <thread>

#include "lakestore/util/thread.hpp"
#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
namespace {
std::string environment(const char* name) {
  const auto* p = std::getenv(name);
  return p ? p : "";
}
CloudConfig config(bool azure) {
  CloudConfig c;
  c.provider = azure ? CloudConfig::Provider::Azure : CloudConfig::Provider::S3;
  c.container = "lakestore-tests";
  c.prefix = "run-" + uuid();
  if (azure) {
    c.endpoint = environment("LAKESTORE_AZURE_ENDPOINT");
    c.account = "devstoreaccount1";
    c.account_key =
        "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==";
  } else {
    c.endpoint = environment("LAKESTORE_S3_ENDPOINT");
    c.access_key = "lakestore";
    c.secret_key = "lakestore-local-password";
  }
  return c;
}
}  // namespace
class CloudConformance : public ::testing::TestWithParam<bool> {
 protected:
  std::shared_ptr<ObjectStore> store;
  void SetUp() override {
    auto c = config(GetParam());
    ASSERT_TRUE(initialize_cloud(c));
    store = cloud_store(c);
  }
  void TearDown() override {
    if (store) {
      auto keys = store->list("");
      if (keys)
        for (auto& key : keys.value()) store->erase(key.key);
    }
  }
};
TEST_P(CloudConformance, ConditionalWritesRangeAndDelete) {
  auto m = store->put_if_absent("a", "abcdef").value();
  EXPECT_EQ(store->put_if_absent("a", "other").error().code, ErrorCode::AlreadyExists);
  EXPECT_EQ(store->get_range("a", 2, 3).value(), "cde");
  EXPECT_EQ(store->get_range("a", 6, 0).value(), "");
  EXPECT_FALSE(store->get_range("a", 5, 2));
  EXPECT_EQ(store->head("a").value().size, 6);
  EXPECT_EQ(store->list("a").value().at(0).meta.etag, store->head("a").value().etag);
  EXPECT_EQ(store->put_if_match("a", "new", "\"wrong\"").error().code,
            ErrorCode::PreconditionFailed);
  EXPECT_TRUE(store->put_if_match("a", "new", m.etag));
  EXPECT_EQ(store->get("a").value(), "new");
  EXPECT_FALSE(store->put_if_match("missing", "v", m.etag));
  EXPECT_TRUE(store->erase("a"));
  EXPECT_TRUE(store->erase("a"));
  EXPECT_EQ(store->get("a").error().code, ErrorCode::NotFound);
}
TEST_P(CloudConformance, PaginationBeyondOneThousand) {
  std::atomic<int> failures{0};
  std::atomic<int> next{0};
  std::vector<JoiningThread> workers;
  for (int w = 0; w < 8; ++w)
    workers.emplace_back([&] {
      for (;;) {
        auto i = next.fetch_add(1);
        if (i >= 1005) return;
        if (!store->put_if_absent("page/" + std::to_string(i), "value")) ++failures;
      }
    });
  workers.clear();
  ASSERT_EQ(failures, 0);
  auto keys = store->list("page/").value();
  EXPECT_EQ(keys.size(), 1005);
  for (auto& k : keys) {
    EXPECT_TRUE(k.key.starts_with("page/"));
    EXPECT_EQ(k.meta.size, 5);
    EXPECT_GT(k.meta.modified_ms, 0);
  }
}
TEST_P(CloudConformance, ConcurrentWritersAndTimeoutResolution) {
  auto faults = std::make_shared<FaultInjectingStore>(store);
  TableStore db(faults, 3);
  db.create("t", simple_schema());
  std::atomic<int> failures{0};
  std::vector<JoiningThread> workers;
  for (int w = 0; w < 6; ++w)
    workers.emplace_back([&, w] {
      try {
        db.append("t", simple_rows(w));
      } catch (...) {
        ++failures;
      }
    });
  workers.clear();
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(db.scan("t").rows.size(), 6);
  auto v = db.snapshot("t").version + 1;
  faults->inject({"put_if_absent", log_key("t", v), ErrorCode::Timeout, true});
  EXPECT_EQ(db.append("t", simple_rows(9)), v);
  EXPECT_EQ(db.scan("t").rows.size(), 7);
  db.verify("t");
}
TEST_P(CloudConformance, CloneRestoreExpireAndOfflineGC) {
  TableStore db(store, 3);
  db.create("t", simple_schema());
  auto v = db.append("t", simple_rows(1));
  db.clone("t", v, "c");
  db.overwrite("t", simple_rows(2));
  db.expire_keep_last("t", 1);
  db.gc(std::chrono::milliseconds(0), true);
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_EQ(db.scan("c").rows, simple_rows(1));
  db.overwrite("c", simple_rows(3));
  db.expire_keep_last("c", 1);
  db.gc(std::chrono::milliseconds(0), true);
  EXPECT_GT(db.gc(std::chrono::milliseconds(0), true).deleted, 0);
  db.verify("t");
  db.verify("c");
}
INSTANTIATE_TEST_SUITE_P(Emulators, CloudConformance, ::testing::Values(false, true));
int main(int argc, char** argv) {
  if (environment("LAKESTORE_S3_ENDPOINT").empty() ||
      environment("LAKESTORE_AZURE_ENDPOINT").empty())
    return 77;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
