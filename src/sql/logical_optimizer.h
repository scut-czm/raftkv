// TODO: 声明 LogicalOptimizer 类
//       public:  Optimize(plan)
//       private: PredicatePushdown / ColumnPruning / ConstantFolding
//                TryEvalConstant(expr) -> int(1/0/-1)
//                CollectColumns(expr, *cols)
#pragma once

#include <memory>

#include "src/sql/logical_plan.h"

namespace raftsql {

class LogicalOptimizer {
public:
  std::unique_ptr<LogicalPlan> Optimize(std::unique_ptr<LogicalPlan> plan);

private:
  // 规则1：谓词下推（参考 BusTub merge_filter_scan）
  // Filter(pred, Scan(t)) → Scan(t, pred)
  std::unique_ptr<LogicalPlan>
  PredicatePushdown(std::unique_ptr<LogicalPlan> plan);

  // 规则2：列裁剪（参考 BusTub column_pruning）
  // 从 Project 向下传播所需列集合
  std::unique_ptr<LogicalPlan> ColumnPruning(std::unique_ptr<LogicalPlan> plan);
  // 规则3：常量折叠（BusTub 中无，新增）
  // WHERE 1 > 2 → false → 直接返回空
  // WHERE 1 < 2 → true → 消除 Filter 节点
  std::unique_ptr<LogicalPlan>
  ConstantFolding(std::unique_ptr<LogicalPlan> plan);
  // 尝试对常量表达式求值
  // 返回 1=true, 0=false, -1=无法确定
  int TryEvalConstant(const Expr *expr);

  // 收集表达式中引用的列名
  static void CollectColumns(const Expr *expr, std::vector<std::string> *cols);
};
} // namespace raftsql