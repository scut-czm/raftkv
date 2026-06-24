// TODO: 实现 LogicalPlan::ToString（各 PlanType 分支格式化输出）
// TODO:
// 实现所有工厂方法（MakeScan/MakeFilter/MakeProject/MakeAggregate/MakeLimit/MakeEmpty）

#include "src/sql/logical_plan.h"

#include <string>

namespace raftsql {

static std::string IndentStr(int indent) {
  return std::string(indent * 2, ' ');
}

static std::string ExprToString(const Expr *e) {
  if (!e) {
    return "null";
  }
  switch (e->kind) {
  case Expr::Kind::kColumn:
    return e->col_name;
  case Expr::Kind::kLiteral:
    return e->literal;
  case Expr::Kind::kBinOp:
    return "(" + ExprToString(e->left.get()) + " " + e->op + " " +
           ExprToString(e->right.get()) + ")";
  }
  return "";
}
std::string LogicalPlan::ToString(int indent) const {
  std::string result;
  std::string pad = IndentStr(indent);
  switch (type) {
  case raftsql::PlanType::kScan: {
    result = pad + "Scan(" + table_name;
    if (scan_predicate) {
      result += ", pred=" + ExprToString(scan_predicate.get());
    }
    if (!project_columns.empty()) {
      result += ", cols=[";
      for (size_t i = 0; i < project_columns.size(); ++i) {
        if (i > 0)
          result += ",";
        result += project_columns[i];
      }
      result += "]";
    }
    result += ")\n";
    break;
  }
  case raftsql::PlanType::kFilter:
    result = pad + "Filter(" + ExprToString(filter_predicate.get()) + ")\n";
    break;
  case PlanType::kProject: {
    result = pad + "Project(";
    for (size_t i = 0; i < project_columns.size(); ++i) {
      if (i > 0)
        result += ",";
      result += project_columns[i];
    }
    result += ")\n";
    break;
  }
  case PlanType::kAggregate:
    result = pad + "Aggregate(" + agg_func + "(" + agg_column + "))\n";
    break;
  case PlanType::kLimit:
    result = pad + "Limit(" + std::to_string(limit_count) + ")\n";
    break;
  case PlanType::kInsert:
    result = pad + "Insert(" + table_name + ")\n";
    break;
  case PlanType::kUpdate:
    result = pad + "Update(" + table_name + ")\n";
    break;
  case PlanType::kDelete:
    result = pad + "Delete(" + table_name + ")\n";
    break;
  case PlanType::kEmpty:
    result = pad + "Empty\n";
    break;
  }
  for (const auto &child : children) {
    result += child->ToString(indent + 1);
  }
  return result;
}
std::unique_ptr<LogicalPlan> LogicalPlan::MakeScan(std::string table_name) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kScan;
  plan->table_name = std::move(table_name);
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalPlan::MakeFilter(std::unique_ptr<Expr> predicate,
                        std::unique_ptr<LogicalPlan> child) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kFilter;
  plan->filter_predicate = std::move(predicate);
  plan->children.push_back(std::move(child));
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalPlan::MakeProject(std::vector<std::string> columns,
                         std::unique_ptr<LogicalPlan> child) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kProject;
  plan->project_columns = std::move(columns);
  plan->children.push_back(std::move(child));
  return plan;
}
std::unique_ptr<LogicalPlan>
LogicalPlan::MakeAggregate(std::string func, std::string col,
                           std::unique_ptr<LogicalPlan> child) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kAggregate;
  plan->agg_func = std::move(func);
  plan->agg_column = std::move(col);
  plan->children.push_back(std::move(child));
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalPlan::MakeLimit(int64_t count, std::unique_ptr<LogicalPlan> child) {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kLimit;
  plan->limit_count = count;
  plan->children.push_back(std::move(child));
  return plan;
}

std::unique_ptr<LogicalPlan> LogicalPlan::MakeEmpty() {
  auto plan = std::make_unique<LogicalPlan>();
  plan->type = PlanType::kEmpty;
  return plan;
}

} // namespace raftsql