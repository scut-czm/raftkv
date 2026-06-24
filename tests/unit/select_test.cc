// TODO: SelectStar —— SELECT * FROM users → 5 行
// TODO: SelectSpecificColumn —— SELECT name FROM users，只含 name 列
// TODO: SelectWhereEq —— WHERE id=3 → 1 行，验证 name
// TODO: SelectWhereGt —— WHERE age>22 → 3 行
// TODO: SelectWhereLt —— WHERE age<23 → 2 行
// TODO: SelectWhereAndCond —— WHERE age>21 AND age<25 → 3 行
// TODO: SelectNoMatch —— WHERE id=999 → 0 行
// TODO: SelectFromNonExistentTable —— 表不存在返回错误
// TODO: SelectNameEqString —— WHERE name='user3' → 1 行
// TODO: SelectCountStar —— COUNT(*) → 5
// TODO: SelectSumAge —— SUM(age) → 115
// TODO: SelectMaxAge / SelectMinAge —— MAX/MIN 聚合

#include <gtest/gtest.h>

#include "src/sql/sql_executor.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {
class SelectTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    executor_ = std::make_unique<SQLExecutor>(client_.get());
    executor_->Execute(
        "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
    for (int i = 1; i <= 5; ++i) {
      executor_->Execute("INSERT INTO users (id, name, age) VALUES (" +
                         std::to_string(i) + ", 'user" + std::to_string(i) +
                         "', " + std::to_string(20 + i) + ")");
    }
  }

  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SQLExecutor> executor_;
};

TEST_F(SelectTest, SelectStar) {
  auto r = executor_->Execute("SELECT * FROM users");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 5u);
}

TEST_F(SelectTest, SelectSpecificColumn) {
  auto r = executor_->Execute("SELECT name FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 5u);
  for (const auto &row : r.rows) {
    EXPECT_EQ(row.count("name"), 1u);
    EXPECT_EQ(row.count("id"), 0u);
    EXPECT_EQ(row.count("age"), 0u);
  }
}
TEST_F(SelectTest, SelectWhereEq) {
  auto r = executor_->Execute("SELECT * FROM users WHERE id = 3");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("id"), "3");
  EXPECT_EQ(r.rows[0].at("name"), "user3");
  EXPECT_EQ(r.rows[0].at("age"), "23");
}

TEST_F(SelectTest, SelectWhereGt) {
  auto r = executor_->Execute("SELECT * FROM users WHERE age > 22");
  EXPECT_TRUE(r.ok);
  // age: 21,22,23,24,25 → >22 means 23,24,25 = 3 rows
  EXPECT_EQ(r.rows.size(), 3u);
  EXPECT_EQ(r.rows[0].at("id"), "3");
  EXPECT_EQ(r.rows[0].at("name"), "user3");
  EXPECT_EQ(r.rows[0].at("age"), "23");
  EXPECT_EQ(r.rows[1].at("id"), "4");
  EXPECT_EQ(r.rows[1].at("name"), "user4");
  EXPECT_EQ(r.rows[1].at("age"), "24");
  EXPECT_EQ(r.rows[2].at("id"), "5");
  EXPECT_EQ(r.rows[2].at("name"), "user5");
  EXPECT_EQ(r.rows[2].at("age"), "25");
}

TEST_F(SelectTest, SelectWhereLt) {
  auto r = executor_->Execute("SELECT * FROM users WHERE age < 23");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 2u); // 21, 22
  EXPECT_EQ(r.rows[0].at("id"), "1");
  EXPECT_EQ(r.rows[0].at("name"), "user1");
  EXPECT_EQ(r.rows[0].at("age"), "21");
  EXPECT_EQ(r.rows[1].at("id"), "2");
  EXPECT_EQ(r.rows[1].at("name"), "user2");
  EXPECT_EQ(r.rows[1].at("age"), "22");
}

TEST_F(SelectTest, SelectWhereAndCond) {
  auto r =
      executor_->Execute("SELECT * FROM users WHERE age > 21 AND age < 25");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 3u); // 22, 23, 24
  EXPECT_EQ(r.rows[0].at("id"), "2");
  EXPECT_EQ(r.rows[0].at("name"), "user2");
  EXPECT_EQ(r.rows[0].at("age"), "22");
  EXPECT_EQ(r.rows[1].at("id"), "3");
  EXPECT_EQ(r.rows[1].at("name"), "user3");
  EXPECT_EQ(r.rows[1].at("age"), "23");
  EXPECT_EQ(r.rows[2].at("id"), "4");
  EXPECT_EQ(r.rows[2].at("name"), "user4");
  EXPECT_EQ(r.rows[2].at("age"), "24");
}

TEST_F(SelectTest, SelectNoMatch) {
  auto r = executor_->Execute("SELECT * FROM users WHERE id = 999");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.rows.size(), 0u);
}

TEST_F(SelectTest, SelectFromNonExistentTable) {
  auto r = executor_->Execute("SELECT * FROM noexist");
  EXPECT_FALSE(r.ok);
}

TEST_F(SelectTest, SelectNameEqString) {
  auto r = executor_->Execute("SELECT * FROM users WHERE name = 'user3'");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("name"), "user3");
}

TEST_F(SelectTest, SelectCountStar) {
  auto r = executor_->Execute("SELECT COUNT(*) FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "5");
}

TEST_F(SelectTest, SelectSumAge) {
  auto r = executor_->Execute("SELECT SUM(age) FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  int sum = 21 + 22 + 23 + 24 + 25;
  EXPECT_EQ(r.rows[0].at("SUM(age)"), std::to_string(sum));
}

TEST_F(SelectTest, SelectMaxAge) {
  auto r = executor_->Execute("SELECT MAX(age) FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("MAX(age)"), "25");
}

TEST_F(SelectTest, SelectMinAge) {
  auto r = executor_->Execute("SELECT MIN(age) FROM users");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("MIN(age)"), "21");
}

} // namespace raftsql