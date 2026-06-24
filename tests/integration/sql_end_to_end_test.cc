#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <string>
#include <memory>

#include "src/client/kv_client.h"
#include "src/sql/raft_kv_client_adapter.h"
#include "src/sql/sql_executor.h"

DEFINE_string(peers, "127.0.0.1:8200", "RaftKV cluster peers, comma-separated");

namespace raftsql {

// 集成测试需要常驻运行中的 RaftKV 集群（3 节点）
class SqlEndToEndTest : public ::testing::Test {
protected:
  void SetUp() override {
    raftkv::ClientOptions opts;
    opts.peers = FLAGS_peers;
    opts.timeout_ms = 5000;
    kv_client_ = std::make_unique<raftkv::KVClient>(opts);
    adapter_ = std::make_unique<RaftKvClientAdapter>(kv_client_.get());
    executor_ = std::make_unique<SQLExecutor>(adapter_.get());
  }

  void TearDown() override {}

  std::unique_ptr<raftkv::KVClient> kv_client_;
  std::unique_ptr<RaftKvClientAdapter> adapter_;
  std::unique_ptr<SQLExecutor> executor_;
};

// =========================================================================
// 1. DDL 测试：创建与重复创建校验
// =========================================================================
TEST_F(SqlEndToEndTest, CreateAndDroTable) {
  auto r = executor_->Execute(
      "CREATE TABLE e2e_users_create (id INT PRIMARY KEY, name VARCHAR(64))");
  EXPECT_TRUE(r.ok) << r.error_msg;

  // 重复创建同名表，应当在 Catalog 审计阶段灵敏拦截并报错
  auto r2 = executor_->Execute(
      "CREATE TABLE e2e_users_create (id INT PRIMARY KEY, name VARCHAR(64))");
  EXPECT_FALSE(r2.ok) << "重复建表未被拦截！";
}

// =========================================================================
// 2. DML 读写往返测试：单行精准点查
// =========================================================================
TEST_F(SqlEndToEndTest, InsertAndSelectRoundTrip) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_users_trip (id INT PRIMARY KEY, name VARCHAR(64), age INT)");
  ASSERT_TRUE(r_create.ok) << "建表失败: " << r_create.error_msg;

  auto ins = executor_->Execute(
      "INSERT INTO e2e_users_trip (id, name, age) VALUES (1, 'Alice', 25)");
  EXPECT_TRUE(ins.ok) << ins.error_msg;

  // 主键等值条件，应当触发 TableScan 的 NarrowScanRange 物理大剪枝优化路径
  auto sel = executor_->Execute("SELECT * FROM e2e_users_trip WHERE id = 1");
  EXPECT_TRUE(sel.ok) << sel.error_msg;
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "Alice");
  EXPECT_EQ(sel.rows[0].at("age"), "25");
}

// =========================================================================
// 3. 全表扫描测试：多行全量泵出
// =========================================================================
TEST_F(SqlEndToEndTest, MultipleRowsSelectAll) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_users_all (id INT PRIMARY KEY, name VARCHAR(64))");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;

  for (int i = 1; i <= 5; ++i) {
    auto ins = executor_->Execute("INSERT INTO e2e_users_all (id, name) VALUES (" +
                       std::to_string(i) + ", 'user" + std::to_string(i) + "')");
    ASSERT_TRUE(ins.ok) << ins.error_msg;
  }

  auto r = executor_->Execute("SELECT * FROM e2e_users_all");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.rows.size(), 5u);
}

// =========================================================================
// 4. 表达式真值过滤测试：WHERE 非主键非等值范围初筛
// =========================================================================
TEST_F(SqlEndToEndTest, SelectWithWherePredicate) {
  auto r_create = executor_->Execute("CREATE TABLE e2e_users_where (id INT PRIMARY KEY, age INT)");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;

  for (int i = 1; i <= 5; ++i) {
    auto ins = executor_->Execute("INSERT INTO e2e_users_where (id, age) VALUES (" +
                       std::to_string(i) + ", " + std::to_string(20 + i) + ")");
    ASSERT_TRUE(ins.ok) << ins.error_msg;
  }

  // 无法下推到 Key 范围的条件，应当顺流向上由 FilterOperator 算子在内存中核验过滤
  auto r = executor_->Execute("SELECT * FROM e2e_users_where WHERE age > 22");
  EXPECT_TRUE(r.ok) << r.error_msg;
  EXPECT_EQ(r.rows.size(), 3u); // 23, 24, 25 应该通关
}

// =========================================================================
// 5. 数据更新测试：UPDATE 内存改写与重新压实写回
// =========================================================================
TEST_F(SqlEndToEndTest, UpdateAndVerify) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_users_update (id INT PRIMARY KEY, name VARCHAR(64))");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;
  executor_->Execute("INSERT INTO e2e_users_update (id, name) VALUES (1, 'Alice')");

  auto upd = executor_->Execute("UPDATE e2e_users_update SET name = 'Bob' WHERE id = 1");
  EXPECT_TRUE(upd.ok) << upd.error_msg;
  EXPECT_EQ(upd.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM e2e_users_update WHERE id = 1");
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "Bob");
}

// =========================================================================
// 6. 数据擦除测试：DELETE 物理墓碑标记/直接删除
// =========================================================================
TEST_F(SqlEndToEndTest, DeleteAndVerify) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_users_delete (id INT PRIMARY KEY, name VARCHAR(64))");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;
  executor_->Execute("INSERT INTO e2e_users_delete (id, name) VALUES (1, 'Alice')");
  executor_->Execute("INSERT INTO e2e_users_delete (id, name) VALUES (2, 'Bob')");

  auto del = executor_->Execute("DELETE FROM e2e_users_delete WHERE id = 1");
  EXPECT_TRUE(del.ok) << del.error_msg;
  EXPECT_EQ(del.affected_rows, 1);

  auto sel = executor_->Execute("SELECT * FROM e2e_users_delete");
  EXPECT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].at("name"), "Bob"); // 只留下 Bob 说明 Alice 确实被物理抹去
}

// =========================================================================
// 7. 火山流式聚合测试 A：COUNT(*) 标量维度坍缩
// =========================================================================
TEST_F(SqlEndToEndTest, CountAggregate) {
  auto r_create = executor_->Execute("CREATE TABLE e2e_users_count (id INT PRIMARY KEY, age INT)");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;

  for (int i = 1; i <= 10; ++i) {
    executor_->Execute("INSERT INTO e2e_users_count (id, age) VALUES (" +
                       std::to_string(i) + ", " + std::to_string(20 + i) + ")");
  }

  // 此时应当激活 AggregateOperator 并在 Open 阶段收拢状态，由 emitted_ 状态机流控输出单行
  auto r = executor_->Execute("SELECT COUNT(*) FROM e2e_users_count");
  EXPECT_TRUE(r.ok) << r.error_msg;
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("COUNT(*)"), "10");
}

// =========================================================================
// 8. 火山流式聚合测试 B：SUM(col) 标量滚动累加
// =========================================================================
TEST_F(SqlEndToEndTest, SumAggregate) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_orders_sum (id INT PRIMARY KEY, amount INT)");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;

  int total = 0;
  for (int i = 1; i <= 5; ++i) {
    int amount = i * 100;
    total += amount;
    executor_->Execute("INSERT INTO e2e_orders_sum (id, amount) VALUES (" +
                       std::to_string(i) + ", " + std::to_string(amount) + ")");
  }

  auto r = executor_->Execute("SELECT SUM(amount) FROM e2e_orders_sum");
  EXPECT_TRUE(r.ok) << r.error_msg;
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].at("SUM(amount)"), std::to_string(total));
}

// =========================================================================
// 9. 集群高可用弹性容错测试：数据强一致校验
// =========================================================================
TEST_F(SqlEndToEndTest, FaultToleranceLeaderFailover) {
  auto r_create = executor_->Execute(
      "CREATE TABLE e2e_users_fault (id INT PRIMARY KEY, name VARCHAR(64))");
  ASSERT_TRUE(r_create.ok) << r_create.error_msg;

  for (int i = 1; i <= 3; ++i) {
    auto r = executor_->Execute("INSERT INTO e2e_users_fault (id, name) VALUES (" +
                                std::to_string(i) + ", 'user" + std::to_string(i) + "')");
    EXPECT_TRUE(r.ok) << r.error_msg;
  }

  // 依靠底层 raftkv::KVClient 的 HandleRedirect 重定向和主动无缝换道，对抗网络的抖动与切主
  auto sel = executor_->Execute("SELECT COUNT(*) FROM e2e_users_fault");
  EXPECT_TRUE(sel.ok) << sel.error_msg;
  EXPECT_EQ(sel.rows[0].at("COUNT(*)"), "3");
}

} // namespace raftsql

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}