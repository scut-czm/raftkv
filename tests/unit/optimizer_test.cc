#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "src/sql/kv_client_interface.h" // 精准对齐你的基类头文件路径
#include "src/sql/logical_optimizer.h"
#include "src/sql/parser.h"
#include "src/sql/planner.h"
#include "src/sql/schema_manager.h"

namespace raftsql {

// =========================================================================
// 🛡️ 1. 物理有序内存 KV 桩：彻底闭合纯虚表（Scan/Put/Get/Delete），击碎 Segfault
// =========================================================================
class FakeKvClient : public KvClientInterface {
public:
  FakeKvClient() = default;
  ~FakeKvClient() override = default;

  // 写入对齐
  bool Put(const std::string &key, const std::string &value) override {
    kv_store_[key] = value;
    return true;
  }

  // 读取对齐
  std::string Get(const std::string &key) override {
    auto it = kv_store_.find(key);
    if (it != kv_store_.end()) {
      return it->second;
    }
    return "";
  }

  // 删除对齐
  bool Delete(const std::string &key) override {
    kv_store_.erase(key);
    return true;
  }

  // 范围扫描精准对齐（完美支持 SchemaManager::ListTables 的前缀扫描）
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit = 10000) override {
    std::vector<std::pair<std::string, std::string>> result;
    auto it = kv_store_.lower_bound(start_key);
    while (it != kv_store_.end() && it->first < end_key &&
           result.size() < static_cast<size_t>(limit)) {
      result.push_back(*it);
      ++it;
    }
    return result;
  }

private:
  std::map<std::string, std::string> kv_store_; // 有序存储矩阵
};

// =========================================================================
// 🏢 2. 单元测试夹具 (Test Fixture)：破除元数据真空，精准注入 Mock 图纸
// =========================================================================
class PlannerOptimizerTest : public ::testing::Test {
protected:
  // 显式初始化列表：安全挂载内存桩地址
  PlannerOptimizerTest() : fake_kv_(), schema_manager_(&fake_kv_) {}

  void SetUp() override {
    // 🏗️ A. 动态编织 "users" 核心测试表元数据 (包含 id, name, age 并设置表名)
    raftsql::TableSchema users_schema;
    users_schema.set_table_name(
        "users"); // 🛡️ 必须显式设置，供 CreateTable 内部读取

    auto *col_id = users_schema.add_columns();
    col_id->set_name("id");
    auto *col_name = users_schema.add_columns();
    col_name->set_name("name");
    auto *col_age = users_schema.add_columns();
    col_age->set_name("age");

    // 🏗️ B. 动态编织 "t" 基础测试表元数据 (供常量折叠测试保底)
    raftsql::TableSchema t_schema;
    t_schema.set_table_name("t");
    auto *col_t_id = t_schema.add_columns();
    col_t_id->set_name("id");

    // 🛡️ C. 唤醒真实的工业级 API 物理灌入元数据大管家
    schema_manager_.CreateTable(users_schema);
    schema_manager_.CreateTable(t_schema);
  }

  void TearDown() override {}

  FakeKvClient fake_kv_;         // 内存沙箱
  SchemaManager schema_manager_; // 元数据仓库
};

// =========================================================================
// 🚀 3. 全量 RBO 逻辑变轨单测靶场 (基于 TEST_F 夹具与刚性防御)
// =========================================================================

TEST_F(PlannerOptimizerTest, SelectStarProducesScan) {
  Parser parser("SELECT * FROM users");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr); // 刚性防空垫，绝不放行空指针向下污染

  EXPECT_EQ(plan->type, PlanType::kScan);
  EXPECT_EQ(plan->table_name, "users");
}

TEST_F(PlannerOptimizerTest, SelectWhereProducesFilterScan) {
  Parser parser("SELECT * FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  EXPECT_EQ(plan->type, PlanType::kFilter);
  ASSERT_EQ(plan->children.size(), 1u);
  EXPECT_EQ(plan->children[0]->type, PlanType::kScan);
}

TEST_F(PlannerOptimizerTest, SelectColumnsProducesProject) {
  Parser parser("SELECT name, age FROM users");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  EXPECT_EQ(plan->type, PlanType::kProject);
  ASSERT_EQ(plan->project_columns.size(), 2u);
  EXPECT_EQ(plan->project_columns[0], "name");
  EXPECT_EQ(plan->project_columns[1], "age");
}

TEST_F(PlannerOptimizerTest, PredicatePushdown) {
  Parser parser("SELECT * FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kScan);
  EXPECT_NE(optimized->scan_predicate, nullptr);
  EXPECT_EQ(optimized->scan_predicate->op, ">");
}

TEST_F(PlannerOptimizerTest, PredicatePushdownWithProject) {
  Parser parser("SELECT name FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  EXPECT_EQ(plan->type, PlanType::kProject);
  EXPECT_EQ(plan->children[0]->type, PlanType::kFilter);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kProject);
  ASSERT_EQ(optimized->children.size(), 1u);
  EXPECT_EQ(optimized->children[0]->type, PlanType::kScan);
  EXPECT_NE(optimized->children[0]->scan_predicate, nullptr);
}

TEST_F(PlannerOptimizerTest, PredicatePushdownNoEffect) {
  auto scan = LogicalPlan::MakeScan("users");
  std::vector<std::string> cols = {"name", "age"};
  auto project = LogicalPlan::MakeProject(cols, std::move(scan));

  auto pred =
      Expr::MakeBinOp(">", Expr::MakeColumn("age"), Expr::MakeLiteral("18"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(project));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kFilter);
  ASSERT_EQ(optimized->children.size(), 1u);
  EXPECT_EQ(optimized->children[0]->type, PlanType::kProject);
}

TEST_F(PlannerOptimizerTest, ColumnPruning) {
  Parser parser("SELECT name FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));
  ASSERT_NE(optimized, nullptr);

  ASSERT_EQ(optimized->type, PlanType::kProject);
  auto &scan = optimized->children[0];
  ASSERT_EQ(scan->type, PlanType::kScan);

  ASSERT_EQ(scan->project_columns.size(), 2u);
  std::set<std::string> actual_cols(scan->project_columns.begin(),
                                    scan->project_columns.end());
  std::set<std::string> expected_cols = {"name", "age"};
  EXPECT_EQ(actual_cols, expected_cols);
}

TEST_F(PlannerOptimizerTest, ConstantFoldingTrue) {
  auto scan = LogicalPlan::MakeScan("t");
  auto pred =
      Expr::MakeBinOp("<", Expr::MakeLiteral("1"), Expr::MakeLiteral("2"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(scan));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kScan);
}

TEST_F(PlannerOptimizerTest, ConstantFoldingFalse) {
  auto scan = LogicalPlan::MakeScan("t");
  auto pred =
      Expr::MakeBinOp(">", Expr::MakeLiteral("1"), Expr::MakeLiteral("2"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(scan));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kEmpty);
}

TEST_F(PlannerOptimizerTest, PlanToString) {
  Parser parser("SELECT name FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  std::string str = plan->ToString();
  EXPECT_NE(str.find("Project"), std::string::npos);
  EXPECT_NE(str.find("Filter"), std::string::npos);
  EXPECT_NE(str.find("Scan"), std::string::npos);
}

TEST_F(PlannerOptimizerTest, CombinedOptimization) {
  Parser parser("SELECT name FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  EXPECT_EQ(plan->type, PlanType::kProject);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));
  ASSERT_NE(optimized, nullptr);

  EXPECT_EQ(optimized->type, PlanType::kProject);
  ASSERT_EQ(optimized->children.size(), 1u);

  auto &scan = optimized->children[0];
  EXPECT_EQ(scan->type, PlanType::kScan);
  EXPECT_NE(scan->scan_predicate, nullptr);

  ASSERT_EQ(scan->project_columns.size(), 2u);
  std::set<std::string> actual_cols(scan->project_columns.begin(),
                                    scan->project_columns.end());
  std::set<std::string> expected_cols = {"name", "age"};
  EXPECT_EQ(actual_cols, expected_cols);
}

} // namespace raftsql