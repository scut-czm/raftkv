// TODO: CountStar —— SELECT COUNT(*) → 5
// TODO: CountStarWithWhere —— WHERE amount>100 → 3
// TODO: SumAmount —— SUM(amount) → 800
// TODO: MaxAmount —— MAX(amount) → 300
// TODO: MinAmount —— MIN(amount) → 50
// TODO: SumWithWhere —— WHERE amount>=150 → SUM=650
// TODO: CountStarOnEmpty —— 空表 COUNT(*) → 0
// TODO: MaxOnEmpty —— 空表 MAX(val) → NULL

#include <gtest/gtest.h>

#include "src/sql/sql_executor.h"
#include "tests/unit/mock_kv_client.h"

namespace raftsql {

class AggregateTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_ = std::make_unique<MockKvClient>();
    executor_ = std::make_unique<SQLExecutor>(client_.get());
    executor_->Execute("CREATE TABLE sales (id INT PRIMARY KEY, amount INT, "
                       "region VARCHAR(32))");
    executor_->Execute(
        "INSERT INTO sales (id, amount, region) VALUES (1, 100, 'north')");
    executor_->Execute(
        "INSERT INTO sales (id, amount, region) VALUES (2, 200, 'south')");
    executor_->Execute(
        "INSERT INTO sales (id, amount, region) VALUES (3, 150, 'north')");
    executor_->Execute(
        "INSERT INTO sales (id, amount, region) VALUES (4, 300, 'south')");
    executor_->Execute(
        "INSERT INTO sales (id, amount, region) VALUES (5, 50, 'west')");
  }

  std::unique_ptr<MockKvClient> client_;
  std::unique_ptr<SQLExecutor> executor_;
};

TEST_F(AggregateTest, CountStar) {
  auto r = executor_->Execute("SELECT COUNT(*) FROM sales");
  EXPECT_TRUE(r.ok) << r.error_msg;
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "5");
}
TEST_F(AggregateTest, CountStarWithWhere) {
  auto r = executor_->Execute("SELECT COUNT(*) FROM sales WHERE amount > 100");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "3");
}

TEST_F(AggregateTest, SumAmount) {
  auto r = executor_->Execute("SELECT SUM(amount) FROM sales");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  int expected = 100 + 200 + 150 + 300 + 50;
  EXPECT_EQ(r.rows[0].at("SUM(amount)"), std::to_string(expected));
}

TEST_F(AggregateTest, MaxAmount) {
  auto r = executor_->Execute("SELECT MAX(amount) FROM sales");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("MAX(amount)"), "300");
}

TEST_F(AggregateTest, MinAmount) {
  auto r = executor_->Execute("SELECT MIN(amount) FROM sales");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("MIN(amount)"), "50");
}

TEST_F(AggregateTest, SumWithWhere) {
  auto r =
      executor_->Execute("SELECT SUM(amount) FROM sales WHERE amount >= 150");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  int expected = 200 + 150 + 300;
  EXPECT_EQ(r.rows[0].at("SUM(amount)"), std::to_string(expected));
}
TEST_F(AggregateTest, CountStarOnEmpty) {
  executor_->Execute("CREATE TABLE empty_table (id INT)");
  auto r = executor_->Execute("SELECT COUNT(*) FROM empty_table");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "0");
}

TEST_F(AggregateTest, MaxOnEmpty) {
  executor_->Execute("CREATE TABLE empty_table (id INT, val INT)");
  auto r = executor_->Execute("SELECT MAX(val) FROM empty_table");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("MAX(val)"), "NULL");
}

} // namespace raftsql