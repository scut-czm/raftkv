#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

#include "src/sql/logical_optimizer.h"
#include "src/sql/parser.h"
#include "src/sql/planner.h"
#include "src/sql/schema_manager.h" // 确保引入元数据管理器

namespace raftsql {

// =========================================================================
// 单元测试夹具 (Test Fixture)：负责统一构建并隔离常驻内存的元数据图纸
// =========================================================================
class PlannerOptimizerTest : public ::testing::Test {
protected:
  // 🛡️ 【核心修正点】显式编写构造函数，在初始化列表中将 nullptr 注入
  // schema_manager_ 彻底消除“没有匹配的 SchemaManager() 构造函数”的编译死锁
  PlannerOptimizerTest() : schema_manager_(nullptr) {}

  void SetUp() override {
    // ---------------------------------------------------------------------
    // 💡 极客提示：在此处向你的全局 schema_manager_ 注入单测所需的虚拟表结构。
    // 假设你的 SchemaManager 支持编排注册，或者通过模拟方式注入以下结构：
    //   1. 表 "users"  -> 包含列: id, name, age
    //   2. 表 "t"      -> 包含列: id (供恒真/恒假等手动搭树单测保底使用)
    // 请根据你本地 SchemaManager 的具体显式注册接口（如
    // AddTable/RegisterTable） 补齐下面这笔 mock 装配逻辑：
    // ---------------------------------------------------------------------

    // 【示例伪代码，请根据本地元数据仓库接口微调】
    // TableSchema users_meta;
    // users_meta.add_column("id", DataType::INT);
    // users_meta.add_column("name", DataType::VARCHAR);
    // users_meta.add_column("age", DataType::INT);
    // schema_manager_.RegisterTable("users", users_meta);

    // TableSchema t_meta;
    // t_meta.add_column("id", DataType::INT);
    // schema_manager_.RegisterTable("t", t_meta);
  }

  void TearDown() override {}

  // 声明全局单测共享的元数据大管家
  SchemaManager schema_manager_;
};

// =========================================================================
// 1. Planner 基础状态拓扑测试 (升级为 TEST_F 挂载夹具)
// =========================================================================

TEST_F(PlannerOptimizerTest, SelectStarProducesScan) {
  Parser parser("SELECT * FROM users");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  // 【核心修正】强行注入元数据依赖，通过编译期语义审计
  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  ASSERT_NE(plan, nullptr);

  // SELECT * 且无 WHERE 时，应当直接翻译为单节点 Scan
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

  // 初始未优化拓扑：Filter -> Scan
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

  // 指定列时，最外层包裹 Project
  EXPECT_EQ(plan->type, PlanType::kProject);
  ASSERT_EQ(plan->project_columns.size(), 2u);
  EXPECT_EQ(plan->project_columns[0], "name");
  EXPECT_EQ(plan->project_columns[1], "age");
}

// =========================================================================
// 2. Optimizer 逻辑优化规则测试 (RBO 谓词下推与列裁剪)
// =========================================================================

TEST_F(PlannerOptimizerTest, PredicatePushdown) {
  Parser parser("SELECT * FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));

  // 优化后拓扑：独立的 Filter 消除，谓词成功坍缩合并到叶子节点 Scan 中
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

  // 优化前验证：Project → Filter → Scan
  EXPECT_EQ(plan->type, PlanType::kProject);
  EXPECT_EQ(plan->children[0]->type, PlanType::kFilter);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));

  // 优化后验证拓扑：Project → Scan (中间阻碍 I/O 的 Filter 被无情剥离抽掉)
  EXPECT_EQ(optimized->type, PlanType::kProject);
  ASSERT_EQ(optimized->children.size(), 1u);
  EXPECT_EQ(optimized->children[0]->type, PlanType::kScan);
  EXPECT_NE(optimized->children[0]->scan_predicate, nullptr);
}

TEST_F(PlannerOptimizerTest, PredicatePushdownNoEffect) {
  // 手动搭树：构造叶子 Scan
  auto scan = LogicalPlan::MakeScan("users");

  // 中间人为制造一个 Project 隔离带，用于阻断下推条件
  std::vector<std::string> cols = {"name", "age"};
  auto project = LogicalPlan::MakeProject(cols, std::move(scan));

  // 最顶层挂载 Filter 算子
  auto pred =
      Expr::MakeBinOp(">", Expr::MakeColumn("age"), Expr::MakeLiteral("18"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(project));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));

  // 验证规则：根节点依然顽固保持为 Filter，证明下推被安全隔离，未发生越权坍缩
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

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));

  ASSERT_EQ(optimized->type, PlanType::kProject);
  auto &scan = optimized->children[0];
  ASSERT_EQ(scan->type, PlanType::kScan);

  // 【强力全量对齐】Scan 收集到的列必须去重且恰好包含【输出列
  // name】+【过滤依赖列 age】
  ASSERT_EQ(scan->project_columns.size(), 2u);
  std::set<std::string> actual_cols(scan->project_columns.begin(),
                                    scan->project_columns.end());
  std::set<std::string> expected_cols = {"name", "age"};
  EXPECT_EQ(actual_cols, expected_cols);
}

TEST_F(PlannerOptimizerTest, ConstantFoldingTrue) {
  // WHERE 1 < 2 → 恒真 → Filter 节点在规划期直接消融
  auto scan = LogicalPlan::MakeScan("t");
  auto pred =
      Expr::MakeBinOp("<", Expr::MakeLiteral("1"), Expr::MakeLiteral("2"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(scan));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));

  // Filter 已完全消除，直接暴露出最纯净的底层 Scan 节点
  EXPECT_EQ(optimized->type, PlanType::kScan);
}

TEST_F(PlannerOptimizerTest, ConstantFoldingFalse) {
  // WHERE 1 > 2 → 恒假 → 切断整棵计划子树
  auto scan = LogicalPlan::MakeScan("t");
  auto pred =
      Expr::MakeBinOp(">", Expr::MakeLiteral("1"), Expr::MakeLiteral("2"));
  auto filter = LogicalPlan::MakeFilter(std::move(pred), std::move(scan));

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(filter));

  // 恒假条件：整棵计划树直接坍缩为最高效的特化 kEmpty 节点，拒绝执行物理 I/O
  EXPECT_EQ(optimized->type, PlanType::kEmpty);
}

TEST_F(PlannerOptimizerTest, PlanToString) {
  Parser parser("SELECT name FROM users WHERE age > 18");
  auto stmt = parser.Parse();
  ASSERT_TRUE(stmt.has_value());

  // 此时已经可以通过夹具完美访问绑定的 schema_manager_，消除了未定义悬空符号
  // Bug
  Planner planner(schema_manager_);
  auto plan = planner.Plan(*stmt);
  std::string str = plan->ToString();

  // 验证基础的文本格式化输出组件能正确打出关键算子的拓扑印记
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

  EXPECT_EQ(plan->type, PlanType::kProject);

  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(plan));

  // 联合终极规约状态验证：Project(name) → Scan(users, pred="age>18",
  // cols=["age","name"])
  EXPECT_EQ(optimized->type, PlanType::kProject);
  ASSERT_EQ(optimized->children.size(), 1u);

  auto &scan = optimized->children[0];
  EXPECT_EQ(scan->type, PlanType::kScan);
  EXPECT_NE(scan->scan_predicate, nullptr);

  // 精准锁定剪裁后的列状态账本
  ASSERT_EQ(scan->project_columns.size(), 2u);
  std::set<std::string> actual_cols(scan->project_columns.begin(),
                                    scan->project_columns.end());
  std::set<std::string> expected_cols = {"name", "age"};
  EXPECT_EQ(actual_cols, expected_cols);
}

} // namespace raftsql