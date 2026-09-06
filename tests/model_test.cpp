#include <random>

#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
TEST(Model, RandomOperationsPreserveRetainedRowsAndSharedFiles) {
  struct ModelTable {
    std::map<uint64_t, Rows> snapshots;
    uint64_t current = 1, floor = 1;
  };
  for (unsigned seed = 0; seed < 12; ++seed) {
    std::mt19937 rng(seed);
    auto store = std::make_shared<MemoryStore>();
    TableStore db(store, 5);
    db.create("t", simple_schema());
    std::map<std::string, ModelTable> model{{"t", {{{1, {}}}, 1, 1}}};
    unsigned clones = 0;
    for (unsigned step = 0; step < 45; ++step) {
      auto it = model.begin();
      std::advance(it, rng() % model.size());
      auto name = it->first;
      auto& m = it->second;
      auto current = m.snapshots.at(m.current);
      auto operation = rng() % 7;
      if (operation == 0) {
        auto rows = simple_rows(static_cast<int64_t>(seed * 1000 + step));
        auto v = db.append(name, rows);
        current.insert(current.end(), rows.begin(), rows.end());
        m.snapshots[v] = current;
        m.current = v;
      } else if (operation == 1) {
        auto rows = simple_rows(static_cast<int64_t>(step));
        auto v = db.overwrite(name, rows);
        m.snapshots[v] = rows;
        m.current = v;
      } else if (operation == 2) {
        auto retained = m.floor + rng() % (m.current - m.floor + 1);
        auto v = db.restore(name, retained);
        m.snapshots[v] = m.snapshots.at(retained);
        m.current = v;
      } else if (operation == 3 && clones < 4) {
        auto retained = m.floor + rng() % (m.current - m.floor + 1);
        auto target = "clone" + std::to_string(++clones);
        db.clone(name, retained, target);
        model.emplace(target, ModelTable{{{1, m.snapshots.at(retained)}}, 1, 1});
      } else if (operation == 4) {
        auto v = db.expire_keep_last(name, 2);
        m.snapshots[v] = current;
        m.current = v;
        m.floor = std::max(m.floor, v > 2 ? v - 1 : 1);
      } else if (operation == 5) {
        auto v = db.compact(name);
        m.snapshots[v] = current;
        m.current = v;
      } else {
        db.gc(std::chrono::milliseconds(0), true);
        db.gc(std::chrono::milliseconds(0), true);
      }
      for (auto& [table, expected] : model) {
        EXPECT_EQ(sorted(db.scan(table).rows), sorted(expected.snapshots.at(expected.current)))
            << "seed=" << seed << " step=" << step;
        for (uint64_t v = expected.floor; v <= expected.current; ++v) {
          ScanOptions options;
          options.version = v;
          EXPECT_EQ(sorted(db.scan(table, options).rows), sorted(expected.snapshots.at(v)));
          for (auto& [path, _] : db.snapshot(table, v).files)
            EXPECT_TRUE(store->head(path)) << path;
        }
      }
    }
  }
}
