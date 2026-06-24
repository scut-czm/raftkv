// TODO: CreateTable —— 创建表成功
// TODO: CreateTableDuplicate —— 重复创建返回错误
// TODO: InsertAndSelect —— INSERT 1 行后 SELECT * 验证
// TODO: InsertToNonExistentTable —— 表不存在返回错误
// TODO: InsertColumnMismatch —— 列数不匹配返回错误
// TODO: SelectFromEmptyTable —— 空表返回 0 行
// TODO: SelectWhereEq / SelectWhereGt —— WHERE 过滤
// TODO: UpdateSingle / DeleteSingle —— UPDATE/DELETE 验证
// TODO: ParseError —— 无效 SQL 返回错误
// TODO: SelectColumns —— 列投影验证（只返回指定列）
// TODO: CountAggregate —— SELECT COUNT(*) 验证
// TODO: MultipleInserts —— 插入 10 行后 SELECT 验证

#include <gtest/gtest.h>

#include "src/sql/sql_executor.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {
class SqlExecutorTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    executor_ = std::make_unique<SQLExecutor>(client_.get());
  }

  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SQLExecutor> executor_;
};

TEST_F(SqlExecutorTest, CreateTable) {
  auto r = executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
  EXPECT_TRUE(r.ok) << r.error_msg;
}

TEST_F(SqlExecutorTest, CreateTableDuplicate) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))");
  auto r = executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error_msg.find("already exists"), std::string::npos);
}

TEST_F(SqlExecutorTest, InsertAndSelect) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
  auto ins = executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25)");
  EXPECT_TRUE(ins.ok) << ins.error_msg;
  EXPECT_EQ(ins.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM users");
  EXPECT_TRUE(sel.ok);
  EXPECT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "Alice");
}

TEST_F(SqlExecutorTest, InsertToNonExistentTable) {
  auto r = executor_->Execute("INSERT INTO noexist (id, name) VALUES (1, 'x')");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error_msg.find("does not exist"), std::string::npos);
}

TEST_F(SqlExecutorTest, InsertColumnMismatch) {
  executor_->Execute("CREATE TABLE t (id INT, name VARCHAR(64))");
  auto r =
      executor_->Execute("INSERT INTO t (id, name) VALUES (1, 'a', 'extra')");
  EXPECT_FALSE(r.ok);
}

TEST_F(SqlExecutorTest, SelectFromEmptyTable) {
  executor_->Execute("CREATE TABLE empty_t (id INT)");
  auto r = executor_->Execute("SELECT * FROM empty_t");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SqlExecutorTest, SelectWhereEq) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))");
  executor_->Execute("INSERT INTO users (id, name) VALUES (1, 'Alice')");
  executor_->Execute("INSERT INTO users (id, name) VALUES (2, 'Bob')");
  executor_->Execute("INSERT INTO users (id, name) VALUES (3, 'Charlie')");

  auto r = executor_->Execute("SELECT * FROM users WHERE id = 2");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("name"), "Bob");
}

TEST_F(SqlExecutorTest, SelectWhereGt) {
  executor_->Execute("CREATE TABLE users (id INT PRIMARY KEY, age INT)");
  executor_->Execute("INSERT INTO users (id, age) VALUES (1, 25)");
  executor_->Execute("INSERT INTO users (id, age) VALUES (2, 30)");
  executor_->Execute("INSERT INTO users (id, age) VALUES (3, 20)");

  auto r = executor_->Execute("SELECT * FROM users WHERE age > 24");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 2u);
}

TEST_F(SqlExecutorTest, UpdateSingle) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))");
  executor_->Execute("INSERT INTO users (id, name) VALUES (1, 'Alice')");

  auto r = executor_->Execute("UPDATE users SET name = 'ALICE' WHERE id = 1");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM users WHERE id = 1");
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "ALICE");
}

TEST_F(SqlExecutorTest, DeleteSingle) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))");
  executor_->Execute("INSERT INTO users (id, name) VALUES (1, 'Alice')");
  executor_->Execute("INSERT INTO users (id, name) VALUES (2, 'Bob')");

  auto r = executor_->Execute("DELETE FROM users WHERE id = 1");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM users");
  EXPECT_EQ(sel.rows.size(), 1u);
}

TEST_F(SqlExecutorTest, ParseError) {
  auto r = executor_->Execute("INVALID SQL HERE");
  EXPECT_FALSE(r.ok);
}

TEST_F(SqlExecutorTest, SelectColumns) {
  executor_->Execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
  executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25)");

  auto r = executor_->Execute("SELECT name FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_TRUE(r.rows[0].count("name") > 0);
  EXPECT_EQ(r.rows[0].count("id"), 0u);
  EXPECT_EQ(r.rows[0].count("age"), 0u);
}

TEST_F(SqlExecutorTest, CountAggregate) {
  executor_->Execute("CREATE TABLE users (id INT PRIMARY KEY, age INT)");
  for (int i = 1; i <= 5; ++i) {
    executor_->Execute("INSERT INTO users (id, age) VALUES (" +
                       std::to_string(i) + ", " + std::to_string(20 + i) + ")");
  }
  auto r = executor_->Execute("SELECT COUNT(*) FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "5");
}

TEST_F(SqlExecutorTest, MultipleInserts) {
  executor_->Execute(
      "CREATE TABLE items (id INT PRIMARY KEY, val VARCHAR(64))");
  for (int i = 1; i <= 10; ++i) {
    auto ins = executor_->Execute("INSERT INTO items (id, val) VALUES (" +
                                  std::to_string(i) + ", 'v" +
                                  std::to_string(i) + "')");
    EXPECT_TRUE(ins.ok) << "Insert " << i << " failed: " << ins.error_msg;
  }
  auto sel = executor_->Execute("SELECT * FROM items");
  EXPECT_TRUE(sel.ok);
  EXPECT_EQ(sel.rows.size(), 10u);
}

} // namespace raftsql