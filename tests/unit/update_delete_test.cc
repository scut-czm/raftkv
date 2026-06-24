// TODO: UpdateSingleRow —— UPDATE SET name='ALICE' WHERE id=1，验证修改
// TODO: UpdateMultipleRows —— WHERE age>25 → affected_rows=2
// TODO: UpdateNoMatch —— WHERE id=999 → affected_rows=0
// TODO: UpdateNonExistentTable —— 表不存在返回错误
// TODO: DeleteSingleRow —— DELETE WHERE id=2，SELECT 验证剩余 2 行
// TODO: DeleteMultipleRows —— WHERE age>=30 → 删除 2 行
// TODO: DeleteAll —— WHERE id>0 → 清空全表
// TODO: DeleteNoMatch —— WHERE id=999 → affected_rows=0
// TODO: DeleteNonExistentTable —— 表不存在返回错误
// TODO: UpdateThenSelect —— UPDATE 后 SELECT 验证值变化

#include <gtest/gtest.h>

#include "src/sql/sql_executor.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {
class UpdateDeleteTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    executor_ = std::make_unique<SQLExecutor>(client_.get());
    executor_->Execute(
        "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
    executor_->Execute(
        "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25)");
    executor_->Execute(
        "INSERT INTO users (id, name, age) VALUES (2, 'Bob', 30)");
    executor_->Execute(
        "INSERT INTO users (id, name, age) VALUES (3, 'Charlie', 35)");
  }
  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SQLExecutor> executor_;
};

TEST_F(UpdateDeleteTest, UpdateSingleRow) {
  auto r = executor_->Execute("UPDATE users SET name = 'ALICE' WHERE id = 1");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM users WHERE id = 1");
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "ALICE");
}

TEST_F(UpdateDeleteTest, UpdateMultipleRows) {
  auto r = executor_->Execute("UPDATE users SET age = 99 WHERE age > 25");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.affected_rows, 2);

  auto sel = executor_->Execute("SELECT * FROM users WHERE age = 99");
  EXPECT_EQ(sel.rows.size(), 2u);
}

TEST_F(UpdateDeleteTest, UpdateNoMatch) {
  auto r = executor_->Execute("UPDATE users SET name = 'X' WHERE id = 999");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.affected_rows, 0);
}

TEST_F(UpdateDeleteTest, UpdateNonExistentTable) {
  auto r = executor_->Execute("UPDATE noexist SET x = 1 WHERE id = 1");
  EXPECT_FALSE(r.ok);
}

TEST_F(UpdateDeleteTest, DeleteSingleRow) {
  auto r = executor_->Execute("DELETE FROM users WHERE id = 2");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM users");
  EXPECT_EQ(sel.rows.size(), 2u);

  auto sel2 = executor_->Execute("SELECT * FROM users WHERE id = 2");
  EXPECT_EQ(sel2.rows.size(), 0u);
}

TEST_F(UpdateDeleteTest, DeleteMultipleRows) {
  auto r = executor_->Execute("DELETE FROM users WHERE age >= 30");
  EXPECT_TRUE(r.ok);

  auto sel = executor_->Execute("SELECT * FROM users");
  EXPECT_EQ(sel.rows.size(), 1u);
}

TEST_F(UpdateDeleteTest, DeleteAll) {
  auto r = executor_->Execute("DELETE FROM users WHERE id > 0");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.affected_rows, 3);

  auto sel = executor_->Execute("SELECT * FROM users");
  EXPECT_EQ(sel.rows.size(), 0u);
}

TEST_F(UpdateDeleteTest, DeleteNoMatch) {
  auto r = executor_->Execute("DELETE FROM users WHERE id = 999");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.affected_rows, 0);
}

TEST_F(UpdateDeleteTest, DeleteNonExistentTable) {
  auto r = executor_->Execute("DELETE FROM noexist WHERE id = 1");
  EXPECT_FALSE(r.ok);
}

TEST_F(UpdateDeleteTest, UpdateThenSelect) {
  executor_->Execute("UPDATE users SET age = 50 WHERE name = 'Alice'");
  auto r = executor_->Execute("SELECT * FROM users WHERE age = 50");
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("name"), "Alice");
}

} // namespace raftsql