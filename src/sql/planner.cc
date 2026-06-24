// TODO: Planner::Plan() —— std::visit 分派到各 Plan* 方法
// TODO: PlanSelect：MakeScan → MakeFilter(可选) →
// MakeAggregate/MakeProject(可选)
// TODO: PlanInsert / PlanCreate / PlanUpdate / PlanDelete
// TODO: Expr clone_expr lambda（deep copy WHERE 子树）

#include "src/sql/planner.h"

#include <memory>

namespace raftsql {
std::unique_ptr<LogicalPlan> Planner::Plan(const Stmt &stmt) {
  return std::visit(
      [this](const auto &s) -> std::unique_ptr<LogicalPlan> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SelectStmt>) {
          return PlanSelect(s);
        } else if constexpr (std::is_same_v<T, InsertStmt>) {
          return PlanInsert(s);
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
          return PlanUpdate(s);
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
          return PlanDelete(s);
        } else if constexpr (std::is_same_v<T, CreateTableStmt>) {
          return PlanCreate(s);
        } else {
          return nullptr;
        }
      },
      stmt);
}

std::unique_ptr<LogicalPlan> Planner::PlanSelect(const SelectStmt &stmt) {
  // 拿着图纸去对账
  auto schema_opt = schema_manager_.GetSchema(stmt.table_name);
  if (!schema_opt) {
    error_msg_ = "Table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }
  auto schema = *schema_opt;

  auto col_exists = [&](const std::string &col_name) {
    for (const auto &col_def : schema.columns()) {
      if (col_def.name() == col_name)
        return true;
    }
    return false;
  };

  auto plan = LogicalPlan::MakeScan(stmt.table_name);

  if (stmt.where_expr) {
    // 克隆表达式（deep copy）
    // 注：简单处理，直接在 Filter 中存原始指针（由 stmt 持有所有权）
    // 实际上应该 clone，这里用一个轻量实现
    auto clone_expr = [](const Expr *src, auto &self) -> std::unique_ptr<Expr> {
      if (!src)
        return nullptr;
      auto e = std::make_unique<Expr>();
      e->kind = src->kind;
      e->col_name = src->col_name;
      e->literal = src->literal;
      e->op = src->op;
      e->left = self(src->left.get(), self);
      e->right = self(src->right.get(), self);
      return e;
    };
    auto where_clone = clone_expr(stmt.where_expr.get(), clone_expr);
    plan = LogicalPlan::MakeFilter(std::move(where_clone), std::move(plan));
  }
  // 聚合
  if (!stmt.agg_func.empty()) {
    plan = LogicalPlan::MakeAggregate(stmt.agg_func, stmt.agg_column,
                                      std::move(plan));
    return plan;
  }
  // 列投影（非 SELECT *）SELECT 投影列强力截击
  if (!stmt.columns.empty()) {
    for (const auto &col : stmt.columns) {
      if (!col_exists(col)) {
        error_msg_ = "Unknown column '" + col + "' in 'field list'";
        return nullptr; // 彻底终留幽灵方格
      }
    }
    plan = LogicalPlan::MakeProject(stmt.columns, std::move(plan));
  }
  return plan;
}

std::unique_ptr<LogicalPlan> Planner::PlanInsert(const InsertStmt &stmt) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kInsert;
  plan->table_name = stmt.table_name;
  plan->insert_columns = stmt.columns;
  plan->insert_values = stmt.values;
  return plan;
}

std::unique_ptr<LogicalPlan>
Planner::PlanCreate(const CreateTableStmt & /*stmt*/) {
  // CREATE TABLE 不通过逻辑计划处理，直接在 Executor 中执行
  return std::make_unique<LogicalPlan>();
}

std::unique_ptr<LogicalPlan> Planner::PlanUpdate(const UpdateStmt &stmt) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kUpdate;
  plan->table_name = stmt.table_name;
  plan->update_set_clauses = stmt.set_clauses;

  if (stmt.where_expr) {
    auto clone_expr = [](const Expr *src, auto &self) -> std::unique_ptr<Expr> {
      if (!src) {
        return nullptr;
      }
      auto e = std::make_unique<Expr>();
      e->kind = src->kind;
      e->col_name = src->col_name;
      e->literal = src->literal;
      e->op = src->op;
      e->left = self(src->left.get(), self);
      e->right = self(src->right.get(), self);
      return e;
    };
    plan->filter_predicate = clone_expr(stmt.where_expr.get(), clone_expr);
  }
  return plan;
}

std::unique_ptr<LogicalPlan> Planner::PlanDelete(const DeleteStmt &stmt) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kDelete;
  plan->table_name = stmt.table_name;

  if (stmt.where_expr) {
    auto clone_expr = [](const Expr *src, auto &self) -> std::unique_ptr<Expr> {
      if (!src)
        return nullptr;
      auto e = std::make_unique<Expr>();
      e->kind = src->kind;
      e->col_name = src->col_name;
      e->literal = src->literal;
      e->op = src->op;
      e->left = self(src->left.get(), self);
      e->right = self(src->right.get(), self);
      return e;
    };
    plan->filter_predicate = clone_expr(stmt.where_expr.get(), clone_expr);
  }
  return plan;
}
} // namespace raftsql