#include <barrier>
#include <fstream>
#include <random>

#include "lakestore/util/thread.hpp"
#include "test_helpers.hpp"
using namespace lakestore;
using namespace lakestore::test;
TEST(Format, RoundTripTypesNullsAndProjection) {
  auto schema = parse_schema("id:int64,x:double,s:string,ts:timestamp");
  Rows rows{
      {int64_t{-9223372036854775807LL}, -0.0, std::string("a\0b", 3), int64_t{1700000000000LL}},
      {int64_t{9223372036854775807LL}, 1.25, std::string(""), std::monostate{}},
      {std::monostate{}, std::monostate{}, std::monostate{}, int64_t{-1}}};
  auto file = encode_file(schema, rows, 1);
  MemoryStore store;
  store.put("f", file.bytes).value();
  ScanMetrics metrics;
  EXPECT_EQ(read_file_rows(store, "f", schema, {}, {0, 1, 2, 3}, metrics), rows);
  ScanMetrics projected;
  auto r = read_file_rows(store, "f", schema, {}, {2, 0}, projected);
  EXPECT_EQ(r[0], (Row{std::string("a\0b", 3), int64_t{-9223372036854775807LL}}));
  EXPECT_LT(projected.data_bytes_read, metrics.data_bytes_read);
}
TEST(Format, EmptyFile) {
  auto s = simple_schema();
  auto f = encode_file(s, {});
  MemoryStore store;
  store.put("f", f.bytes).value();
  ScanMetrics m;
  EXPECT_TRUE(read_file_rows(store, "f", s, {}, {0, 1}, m).empty());
}
TEST(Format, CRC32CKnownVector) { EXPECT_EQ(crc32c("123456789"), 0xe3069283U); }
TEST(Format, CorruptedChunkAndFooterFail) {
  auto s = simple_schema();
  auto f = encode_file(s, simple_rows(1));
  MemoryStore store;
  auto chunk = f.bytes;
  chunk[5] ^= 1;
  store.put("chunk", chunk).value();
  ScanMetrics m;
  expect_error(ErrorCode::Corruption, [&] { read_file_rows(store, "chunk", s, {}, {0, 1}, m); });
  auto footer = f.bytes;
  footer[footer.size() - 20] ^= 1;
  store.put("footer", footer).value();
  expect_error(ErrorCode::Corruption, [&] { read_footer(store, "footer"); });
}
TEST(Format, RejectInvalidSizesAndTruncation) {
  MemoryStore store;
  auto f = encode_file(simple_schema(), simple_rows(1));
  for (size_t n = 0; n < f.bytes.size(); ++n) {
    store.put("f", std::string_view(f.bytes).substr(0, n)).value();
    expect_error(ErrorCode::Corruption, [&] { read_footer(store, "f"); });
  }
  auto b = f.bytes;
  for (size_t i = b.size() - 16; i < b.size() - 8; ++i) b[i] = static_cast<char>(255);
  store.put("f", b).value();
  expect_error(ErrorCode::Corruption, [&] { read_footer(store, "f"); });
}
TEST(Format, RejectSchemaAndValueMismatch) {
  EXPECT_THROW(parse_schema("id:int64,id:string"), Failure);
  EXPECT_THROW(parse_schema("x:int64,"), Failure);
  EXPECT_THROW(encode_file(simple_schema(), {{int64_t{1}}}), Failure);
  EXPECT_THROW(encode_file(parse_schema("x:double"), {{std::numeric_limits<double>::infinity()}}),
               Failure);
  EXPECT_THROW(parse_value(Type::Double, "nan"), Failure);
  EXPECT_THROW(parse_value(Type::Int64, "12x"), Failure);
  EXPECT_THROW(json_uint(Json(-1)), Failure);
}
TEST(Predicates, ParserAndStringEscapes) {
  auto s = simple_schema();
  auto p = parse_predicates(s, "id >= 1 AND label = 'it''s AND fine'");
  EXPECT_EQ(p.size(), 2);
  EXPECT_TRUE(matches({int64_t{2}, std::string("it's AND fine")}, p));
  for (auto expr :
       {"id = 1 AND", "nope = 1", "id = 1 OR id = 2", "label = unquoted", "id == 2", "id = 1 junk"})
    EXPECT_THROW(parse_predicates(s, expr), Failure) << expr;
}
TEST(Predicates, MissingAndMalformedStatsStayConservative) {
  auto p = parse_predicates(simple_schema(), "id > 20");
  EXPECT_TRUE(may_match(Json{}, p));
  EXPECT_TRUE(may_match(Json::array({{{"min", "bad"}, {"max", 10}}}), p));
  EXPECT_TRUE(may_match(Json::array({{{"min", 30}, {"max", 10}}}), p));
}
TEST(Pruning, SeededRandomDataEqualsFullScan) {
  auto schema = parse_schema("id:int64,x:double,s:string,ts:timestamp");
  std::mt19937 rng(43812);
  Rows rows;
  for (int i = 0; i < 500; ++i) {
    Row r{static_cast<int64_t>(rng() % 100) - 50, static_cast<double>(rng() % 100) / 3,
          std::string(1, static_cast<char>('a' + rng() % 5)), static_cast<int64_t>(rng() % 1000)};
    if (rng() % 7 == 0) r[rng() % 4] = std::monostate{};
    rows.push_back(std::move(r));
  }
  MemoryStore store;
  store.put("f", encode_file(schema, rows, 17).bytes).value();
  std::vector<std::string> ops{"=", "<", "<=", ">", ">="};
  for (int i = 0; i < 250; ++i) {
    size_t col = rng() % 4;
    Value literal = col == 0   ? Value(static_cast<int64_t>(rng() % 140) - 70)
                    : col == 1 ? Value(static_cast<double>(rng() % 140) / 3)
                    : col == 2 ? Value(std::string(1, static_cast<char>('a' + rng() % 7)))
                               : Value(static_cast<int64_t>(rng() % 1200));
    std::vector<Predicate> predicates{{col, ops[rng() % 5], literal}};
    ScanMetrics a, b;
    auto pruned = read_file_rows(store, "f", schema, predicates, {0, 1, 2, 3}, a, true),
         full = read_file_rows(store, "f", schema, predicates, {0, 1, 2, 3}, b, false);
    EXPECT_EQ(pruned, full);
  }
}
TEST(Csv, QuotingNewlinesNullsAndHeaders) {
  TempDir d;
  std::filesystem::create_directories(d.path);
  auto p = d.path / "data.csv";
  {
    std::ofstream f(p);
    f << "id,label\r\n1,\"hello, \"\"world\"\"\"\r\n2,\"two\nlines\"\r\n3,\\N\r\n";
  }
  auto r = read_csv(simple_schema(), p.string());
  ASSERT_EQ(r.size(), 3);
  EXPECT_EQ(std::get<std::string>(r[0][1]), "hello, \"world\"");
  EXPECT_EQ(std::get<std::string>(r[1][1]), "two\nlines");
  EXPECT_EQ(r[2][1].index(), 0);
}

TEST(Format, DuplicateStringProjectionPreservesValues) {
  auto schema = simple_schema();
  MemoryStore store;
  store.put("f", encode_file(schema, simple_rows(7)).bytes).value();
  ScanMetrics metrics;
  EXPECT_EQ(read_file_rows(store, "f", schema, {}, {1, 1, 0}, metrics),
            (Rows{{std::string("row"), std::string("row"), int64_t{7}}}));
}

TEST(Metadata, SignedIntegerParsingRejectsLossyConversions) {
  for (const auto& j : {Json(1.5), Json(1e300), Json(uint64_t{UINT64_MAX}), Json("12")})
    expect_error(ErrorCode::Corruption, [&] { json_int(j); });
  EXPECT_EQ(json_int(Json(INT64_MIN)), INT64_MIN);
  EXPECT_EQ(json_int(Json(INT64_MAX)), INT64_MAX);
}

TEST(Format, TableCRCMatchesBitwiseReferenceForRandomBinaryInputs) {
  auto reference = [](std::string_view bytes) {
    uint32_t crc = ~uint32_t{0};
    for (char raw : bytes) {
      crc ^= static_cast<unsigned char>(raw);
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
    return ~crc;
  };
  std::mt19937 rng(381);
  for (int trial = 0; trial < 1000; ++trial) {
    std::string bytes(rng() % 8192, '\0');
    for (auto& byte : bytes) byte = static_cast<char>(rng());
    EXPECT_EQ(crc32c(bytes), reference(bytes));
  }
}

TEST(Format, ConcurrentPredicateParsing) {
  const auto schema = parse_schema("id:int64,label:string");
  std::barrier start(16);
  std::atomic<int> failures{0};
  std::vector<JoiningThread> workers;
  for (int i = 0; i < 16; ++i)
    workers.emplace_back([&] {
      start.arrive_and_wait();
      for (int j = 0; j < 20; ++j) {
        try {
          auto predicates = parse_predicates(schema, "id >= 3 AND label = 'x AND y'");
          if (predicates.size() != 2 || std::get<std::string>(predicates[1].literal) != "x AND y")
            ++failures;
        } catch (...) {
          ++failures;
        }
      }
    });
  workers.clear();
  EXPECT_EQ(failures, 0);
}
