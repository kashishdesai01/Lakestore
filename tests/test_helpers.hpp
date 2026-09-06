#pragma once
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "lakestore/table.hpp"
namespace lakestore::test {
struct TempDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("lakestore-" + uuid());
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
template <class F>
void expect_error(ErrorCode code, F fn) {
  try {
    fn();
    FAIL() << "expected typed failure";
  } catch (const Failure& e) {
    EXPECT_EQ(e.error().code, code) << e.what();
  }
}
inline std::vector<std::string> sorted(const Rows& rows) {
  std::vector<std::string> out;
  for (auto& row : rows) out.push_back(row_json(row).dump());
  std::sort(out.begin(), out.end());
  return out;
}
inline Schema simple_schema() { return parse_schema("id:int64,label:string"); }
inline Rows simple_rows(int64_t id) { return {{id, std::string("row")}}; }
}  // namespace lakestore::test
