// TODO: BasicInsert —— INSERT 1 行，affected_rows=1
// TODO: InsertKeyStoredInKv —— 验证 KV 中存在 users:00000001
// TODO: InsertMultipleRows —— INSERT 5 行，KV size >= 5
// TODO: InsertUnknownColumn —— 未知列返回错误
// TODO: InsertColumnCountMismatch —— 列数不匹配返回错误
// TODO: InsertIntoNonExistentTable —— 表不存在返回错误
// TODO: InsertStringWithQuotes —— 含空格的字符串值
// TODO: InsertAndRetrieveIntValues —— INSERT 后 WHERE 查询验证

#include <gtest/gtest.h>

#include "src/sql/sql_executor.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {
class InsertTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    executor_ = std::make_unique<SQLExecutor>(client_.get());
    executor_->Execute(
        "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
  }

  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SQLExecutor> executor_;
};

TEST_F(InsertTest, BasicInsert) {
  auto r = executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25)");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.affected_rows, 1);
}

TEST_F(InsertTest, InsertKeyStoredInKv) {
  executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25)");
  // 行 key = "users:00000001"
  auto val = client_->Get("users:00000001");
  EXPECT_FALSE(val.empty());
}

TEST_F(InsertTest, InsertMultipleRows) {
  for (int i = 1; i <= 5; ++i) {
    auto r = executor_->Execute(
        "INSERT INTO users (id, name, age) VALUES (" + std::to_string(i) +
        ", 'user" + std::to_string(i) + "', " + std::to_string(20 + i) + ")");
    EXPECT_TRUE(r.ok) << r.error_msg;
  }
  EXPECT_GE(client_->Size(), 5u);
}

TEST_F(InsertTest, InsertUnknownColumn) {
  auto r =
      executor_->Execute("INSERT INTO users (id, unknown_col) VALUES (1, 'x')");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error_msg.find("Unknown column"), std::string::npos);
}

TEST_F(InsertTest, InsertColumnCountMismatch) {
  auto r = executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice')");
  EXPECT_FALSE(r.ok);
}

TEST_F(InsertTest, InsertIntoNonExistentTable) {
  auto r = executor_->Execute("INSERT INTO noexist (id) VALUES (1)");
  EXPECT_FALSE(r.ok);
}

TEST_F(InsertTest, InsertStringWithQuotes) {
  auto r = executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'O Brien', 30)");
  EXPECT_TRUE(r.ok) << r.error_msg;

  auto sel = executor_->Execute("SELECT * FROM users WHERE id = 1");
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "O Brien");
}

TEST_F(InsertTest, InsertAndRetrieveIntValues) {
  executor_->Execute(
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 99)");
  auto r = executor_->Execute("SELECT * FROM users WHERE age = 99");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("age"), "99");
}

} // namespace raftsql