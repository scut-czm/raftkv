// TODO: 定义 PlanType
// 枚举（kScan/kFilter/kProject/kAggregate/kLimit/kInsert/kUpdate/kDelete/kEmpty）
// TODO: 定义 LogicalPlan 结构体
//       - type / table_name / scan_predicate / filter_predicate
//       - agg_func / agg_column / limit_count
//       - insert_columns / insert_values / update_set_clauses
//       - children: vector<unique_ptr<LogicalPlan>>
// TODO: 工厂方法
// MakeScan/MakeFilter/MakeProject/MakeAggregate/MakeLimit/MakeEmpty
// TODO: ToString(indent) 调试打印

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/sql/ast.h"

namespace raftsql {
enum class PlanType {
  kScan,      // 全表扫描 / 条件扫描
  kFilter,    // 过滤
  kProject,   // 投影（列选择）
  kAggregate, // 聚合（COUNT/SUM/MIN/MAX）
  kLimit,     // 行数限制
  kInsert,    // 插入
  kUpdate,    // 更新
  kDelete,    // 删除
  kEmpty,     // 空结果（常量折叠：WHERE false）
};

struct LogicalPlan {
  PlanType type;

  // kScan
  std::string table_name;
  std::unique_ptr<Expr> scan_predicate;

  // kFilter
  std::unique_ptr<Expr> filter_predicate;

  // kProject
  std::vector<std::string> project_columns;

  // kAggregate
  std::string agg_func;   // "COUNT" / "SUM" / "MIN" / "MAX"
  std::string agg_column; // 聚合列名

  // kLimit
  int64_t limit_count = -1;

  // kInsert/ kUpdate / kDelete
  std::vector<std::string> insert_columns;
  std::vector<std::string> insert_values;
  std::vector<std::pair<std::string, std::string>> update_set_clauses;

  // 子计划
  std::vector<std::unique_ptr<LogicalPlan>> children;

  // 调试打印
  std::string ToString(int indent = 0) const;

  // 工厂方法
  static std::unique_ptr<LogicalPlan> MakeScan(std::string table_name);
  static std::unique_ptr<LogicalPlan>
  MakeFilter(std::unique_ptr<Expr> predicate,
             std::unique_ptr<LogicalPlan> child);
  static std::unique_ptr<LogicalPlan>
  MakeProject(std::vector<std::string> columns,
              std::unique_ptr<LogicalPlan> child);
  static std::unique_ptr<LogicalPlan>
  MakeAggregate(std::string func, std::string col,
                std::unique_ptr<LogicalPlan> child);
  static std::unique_ptr<LogicalPlan>
  MakeLimit(int64_t count, std::unique_ptr<LogicalPlan> child);
  static std::unique_ptr<LogicalPlan> MakeEmpty();
};
} // namespace raftsql