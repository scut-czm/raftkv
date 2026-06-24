// TODO: Optimize() —— 依次调用 ConstantFolding → PredicatePushdown →
// ColumnPruning
// TODO: PredicatePushdown：Filter(pred, Scan) → Scan(pred)，合并已有谓词用 AND
// TODO: ColumnPruning：Project → Scan，将 project_columns + predicate 列写入
// scan.project_columns
// TODO: ConstantFolding：恒真消除 Filter 节点，恒假替换为 Empty
// TODO: TryEvalConstant：两字面量比较 + AND/OR 递归
// TODO: CollectColumns：递归收集 kColumn 节点的列名

#include "src/sql/logical_optimizer.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

namespace raftsql {
std::unique_ptr<LogicalPlan>
LogicalOptimizer::Optimize(std::unique_ptr<LogicalPlan> plan) {
  plan = ConstantFolding(std::move(plan));
  plan = PredicatePushdown(std::move(plan));
  plan = ColumnPruning(std::move(plan));
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalOptimizer::ConstantFolding(std::unique_ptr<LogicalPlan> plan) {
  if (!plan) {
    return plan;
  }
  // 递归处理子节点
  for (auto &child : plan->children) {
    child = ConstantFolding(std::move(child));
  }
  // 如果是 Filter，尝试评估谓词
  if (plan->type == PlanType::kFilter && plan->filter_predicate) {
    int result = TryEvalConstant(plan->filter_predicate.get());
    if (result == 1) {
      // 恒真，消除 Filter
      return std::move(plan->children[0]);
    }
    if (result == 0) {
      return LogicalPlan::MakeEmpty(); // 恒假，返回空结果
    }
  }
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalOptimizer::PredicatePushdown(std::unique_ptr<LogicalPlan> plan) {
  if (!plan) {
    return plan;
  }
  // 递归处理所有子节点
  for (auto &child : plan->children) {
    child = PredicatePushdown(std::move(child));
  }
  // 如果当前是 Filter，且唯一子节点是 Scan
  if (plan->type == PlanType::kFilter && !plan->children.empty() &&
      plan->children[0]->type == PlanType::kScan) {
    auto &scan = plan->children[0];
    // 将 Filter 谓词合并到 Scan 的 scan_predicate
    if (scan->scan_predicate) {
      // 已有谓词，用 AND 连接
      scan->scan_predicate =
          Expr::MakeBinOp("AND", std::move(scan->scan_predicate),
                          std::move(plan->filter_predicate));
    } else {
      scan->scan_predicate = std::move(plan->filter_predicate);
    }
    return std::move(plan->children[0]);
  }
  return plan;
}

std::unique_ptr<LogicalPlan>
LogicalOptimizer::ColumnPruning(std::unique_ptr<LogicalPlan> plan) {
  if (!plan)
    return plan;
  // 递归处理子节点
  for (auto &child : plan->children) {
    child = ColumnPruning(std::move(child));
  }
  // 如果当前是 Project，且子节点是 Scan
  if (plan->type == PlanType::kProject && !plan->children.empty() &&
      plan->children[0]->type == PlanType::kScan) {
    auto &scan = plan->children[0];
    // 收集 Project 需要的列 + 谓词中引用的列
    std::set<std::string> needed_set(plan->project_columns.begin(),
                                     plan->project_columns.end());

    // 收集谓词中引用的列
    std::vector<std::string> pred_cols;
    if (scan->scan_predicate) {
      CollectColumns(scan->scan_predicate.get(), &pred_cols);
      for (const auto &col : pred_cols) {
        needed_set.insert(col);
      }
    }
    scan->project_columns =
        std::vector<std::string>(needed_set.begin(), needed_set.end());
  }
  return plan;
}

void LogicalOptimizer::CollectColumns(const Expr *expr,
                                      std::vector<std::string> *cols) {
  if (!expr) {
    return;
  }
  if (expr->kind == Expr::Kind::kColumn) {
    cols->push_back(expr->col_name);
    return;
  }
  CollectColumns(expr->left.get(), cols);
  CollectColumns(expr->right.get(), cols);
}

int LogicalOptimizer::TryEvalConstant(const Expr *expr) {
  if (!expr)
    return -1;
  if (expr->kind != Expr::Kind::kBinOp)
    return -1;

  // 只处理两侧都是字面量的情况
  if (expr->left && expr->right && expr->left->kind == Expr::Kind::kLiteral &&
      expr->right->kind == Expr::Kind::kLiteral) {
    // 尝试数字比较
    try {
      int64_t lv = std::stoll(expr->left->literal);
      int64_t rv = std::stoll(expr->right->literal);
      bool result = false;
      if (expr->op == "=") {
        result = (lv == rv);
      } else if (expr->op == "!=")
        result = (lv != rv);
      else if (expr->op == "<")
        result = (lv < rv);
      else if (expr->op == ">")
        result = (lv > rv);
      else if (expr->op == "<=")
        result = (lv <= rv);
      else if (expr->op == ">=")
        result = (lv >= rv);
      else
        return -1;
      return result ? 1 : 0;
    } catch (const std::exception &e) {
      // 字符串比较
      const std::string &lstr = expr->left->literal;
      const std::string &rstr = expr->right->literal;
      bool result = false;
      if (expr->op == "=")
        result = (lstr == rstr);
      else if (expr->op == "!=")
        result = (lstr != rstr);
      else if (expr->op == "<")
        result = (lstr < rstr);
      else if (expr->op == ">")
        result = (lstr > rstr);
      else if (expr->op == "<=")
        result = (lstr <= rstr);
      else if (expr->op == ">=")
        result = (lstr >= rstr);
      else
        return -1;
      return result ? 1 : 0;
    }
  }
  // AND/OR 规则
  if (expr->op == "AND") {
    int lv = TryEvalConstant(expr->left.get());
    int rv = TryEvalConstant(expr->right.get());
    if (lv == 0 || rv == 0)
      return 0;
    if (lv == 1 && rv == 1)
      return 1;
    return -1;
  }
  if (expr->op == "OR") {
    int lv = TryEvalConstant(expr->left.get());
    int rv = TryEvalConstant(expr->right.get());
    if (lv == 1 || rv == 1)
      return 1;
    if (lv == 0 && rv == 0)
      return 0;
    return -1;
  }
  return -1;
}

} // namespace raftsql