// TODO: EncodeRowKey —— 验证 "users:00000001" 格式
// TODO: DecodeRowKey —— 反解析 table 和 row_id
// TODO: DecodeRowKeyInvalid —— 无冒号返回 false
// TODO: TableScanRange —— 范围覆盖所有行
// TODO: EncodeDecodeIntAndVarchar —— INT+VARCHAR 往返编解码
// TODO: EncodeDecodeFloat —— FLOAT 类型
// TODO: NullValue —— 缺失列编码为 NULL
// TODO: KeyOrdering —— row_id 1~100 字典序正确
// TODO: LargeRowId —— row_id=99999999
// TODO: GetPrimaryKeyColumn —— 返回主键列名

#include <gtest/gtest.h>

#include "src/sql/row_codec.h"

namespace raftsql {
TableSchema MakeSchema(const std::string &table_name) {
  TableSchema schema;
  schema.set_table_name(table_name);
  auto *c1 = schema.add_columns();
  c1->set_name("id");
  c1->set_type(DT_INT);
  c1->set_primary_key(true);
  auto *c2 = schema.add_columns();
  c2->set_name("name");
  c2->set_type(DT_VARCHAR);
  auto *c3 = schema.add_columns();
  c3->set_name("age");
  c3->set_type(DT_INT);
  return schema;
}

TEST(RowCodecTest, EncodeRowKey) {
  EXPECT_EQ(RowCodec::EncodeRowKey("users", 1), "users:00000001");
  EXPECT_EQ(RowCodec::EncodeRowKey("users", 42), "users:00000042");
  EXPECT_EQ(RowCodec::EncodeRowKey("orders", 100), "orders:00000100");
}

TEST(RowCodecTest, DecodeRowKey) {
  std::string_view table;
  int64_t row_id;
  ASSERT_TRUE(RowCodec::DecodeRowKey("users:00000001", &table, &row_id));
  EXPECT_EQ(table, "users");
  EXPECT_EQ(row_id, 1);

  ASSERT_TRUE(RowCodec::DecodeRowKey("orders:00000042", &table, &row_id));
  EXPECT_EQ(table, "orders");
  EXPECT_EQ(row_id, 42);
}

TEST(RowCodecTest, DecodeRowKeyInvalid) {
  std::string_view table;
  int64_t row_id;
  EXPECT_FALSE(RowCodec::DecodeRowKey("nocolon", &table, &row_id));
}

TEST(RowCodecTest, EncodeDecodeIntAndVarchar) {
  auto schema = MakeSchema("users");
  Row row = {{"id", "42"}, {"name", "Alice"}, {"age", "25"}};
  std::string encoded = RowCodec::EncodeRow(row, schema);
  EXPECT_FALSE(encoded.empty());

  Row decoded = RowCodec::DecodeRow(encoded, schema);
  EXPECT_EQ(decoded.at("id"), "42");
  EXPECT_EQ(decoded.at("name"), "Alice");
  EXPECT_EQ(decoded.at("age"), "25");
}


TEST(RowCodecTest, EncodeDecodeFloat) {
  TableSchema schema;
  schema.set_table_name("prices");
  auto* c = schema.add_columns();
  c->set_name("price");
  c->set_type(DT_FLOAT);

  Row row = {{"price", "3.14"}};
  auto encoded = RowCodec::EncodeRow(row, schema);
  auto decoded = RowCodec::DecodeRow(encoded, schema);
  EXPECT_FALSE(decoded.at("price").empty());
}

TEST(RowCodecTest, NullValue) {
  auto schema = MakeSchema("users");
  Row row = {{"id", "1"}};  // name 和 age 缺失 → NULL
  auto encoded = RowCodec::EncodeRow(row, schema);
  auto decoded = RowCodec::DecodeRow(encoded, schema);
  EXPECT_EQ(decoded.at("id"), "1");
  EXPECT_EQ(decoded.count("name"), 0u);  // NULL 不写入 decoded row
}


TEST(RowCodecTest, KeyOrdering) {
  std::string prev;
  for (int i = 1; i <= 100; ++i) {
    auto key = RowCodec::EncodeRowKey("t", i);
    if (!prev.empty()) {
      EXPECT_LT(prev, key) << "row_id " << (i - 1) << " vs " << i;
    }
    prev = key;
  }
}

TEST(RowCodecTest, LargeRowId) {
  auto key = RowCodec::EncodeRowKey("t", 99999999);
  EXPECT_EQ(key, "t:99999999");
}

TEST(RowCodecTest, GetPrimaryKeyColumn) {
  auto schema = MakeSchema("users");
  EXPECT_EQ(RowCodec::GetPrimaryKeyColumn(schema), "id");
}



} // namespace raftsql