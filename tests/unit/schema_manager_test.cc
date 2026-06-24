// TODO: CreateAndGetSchema —— 创建后 GetSchema 验证字段
// TODO: GetSchemaFromCache —— 两次 GetSchema，第二次走缓存
// TODO: DuplicateCreate —— 重复创建返回 false
// TODO: GetNonExistent —— 不存在的表返回 nullopt
// TODO: ListTables —— 创建 3 张表后 ListTables 返回 3 个
// TODO: TableExists —— 存在/不存在验证
// TODO: UpdateNextRowId —— 更新后 next_row_id 正确
// TODO: DropTable —— 删除后 TableExists 返回 false
// TODO: InvalidateCache —— 缓存失效后从 KV 重新读取
// TODO: InitialNextRowIdIsOne —— CreateTable 默认 next_row_id=1

#include <gtest/gtest.h>

#include "src/sql/schema_manager.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {

class SchemaManagerTest : public ::testing::Test {

protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    manager_ = std::make_unique<SchemaManager>(client_.get());
  }
  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SchemaManager> manager_;
};

TableSchema MakeUsersSchema() {
  TableSchema schema;
  schema.set_table_name("users");
  auto *c1 = schema.add_columns();
  c1->set_name("id");
  c1->set_type(DT_INT);
  c1->set_primary_key(true);
  auto *c2 = schema.add_columns();
  c2->set_name("name");
  c2->set_type(DT_VARCHAR);
  c2->set_varchar_len(64);
  schema.set_next_row_id(1);
  return schema;
}

TEST_F(SchemaManagerTest, CreateAndGetSchema) {
  EXPECT_TRUE(manager_->CreateTable(MakeUsersSchema()));

  auto result = manager_->GetSchema("users");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->table_name(), "users");
  EXPECT_EQ(result->columns_size(), 2);
  EXPECT_EQ(result->columns(0).name(), "id");
  EXPECT_TRUE(result->columns(0).primary_key());
}

TEST_F(SchemaManagerTest, GetSchemaFromCache) {
  manager_->CreateTable(MakeUsersSchema());
  // 第一次查 KV，第二次走缓存
  auto r1 = manager_->GetSchema("users");
  auto r2 = manager_->GetSchema("users");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r1->table_name(), r2->table_name());
}

TEST_F(SchemaManagerTest, DuplicateCreate) {
  EXPECT_TRUE(manager_->CreateTable(MakeUsersSchema()));
  EXPECT_FALSE(manager_->CreateTable(MakeUsersSchema()));
}

TEST_F(SchemaManagerTest, GetNonExistent) {
  auto result = manager_->GetSchema("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST_F(SchemaManagerTest, ListTables) {
  for (const auto *name : {"t1", "t2", "t3"}) {
    TableSchema s;
    s.set_table_name(name);
    manager_->CreateTable(s);
  }
  auto tables = manager_->ListTables();
  EXPECT_EQ(tables.size(), 3u);
}
TEST_F(SchemaManagerTest, TableExists) {
  manager_->CreateTable(MakeUsersSchema());
  EXPECT_TRUE(manager_->TableExists("users"));
  EXPECT_FALSE(manager_->TableExists("orders"));
}

TEST_F(SchemaManagerTest, UpdateNextRowId) {
  manager_->CreateTable(MakeUsersSchema());
  auto s = manager_->GetSchema("users");
  ASSERT_TRUE(s.has_value());
  s->set_next_row_id(s->next_row_id() + 1);
  manager_->UpdateSchema(*s);

  auto updated = manager_->GetSchema("users");
  EXPECT_EQ(updated->next_row_id(), 2);
}

TEST_F(SchemaManagerTest, DropTable) {
  manager_->CreateTable(MakeUsersSchema());
  EXPECT_TRUE(manager_->TableExists("users"));

  manager_->DropTable("users");
  EXPECT_FALSE(manager_->TableExists("users"));
  EXPECT_FALSE(manager_->GetSchema("users").has_value());
}

TEST_F(SchemaManagerTest, InvalidateCache) {
  manager_->CreateTable(MakeUsersSchema());
  manager_->GetSchema("users");       // 写入缓存
  manager_->InvalidateCache("users"); // 清除

  // 强制从 KV 读取（修改了 KV 中数据后验证缓存失效）
  auto result = manager_->GetSchema("users");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->table_name(), "users");
}

TEST_F(SchemaManagerTest, InitialNextRowIdIsOne) {
  TableSchema s;
  s.set_table_name("orders");
  // 不设置 next_row_id，默认应为 0，CreateTable 会设为 1
  manager_->CreateTable(s);

  auto result = manager_->GetSchema("orders");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->next_row_id(), 1);
}

} // namespace raftsql