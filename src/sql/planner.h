// TODO: 声明 Planner 类
//       public:  Plan(Stmt) / PlanSelect(SelectStmt)
//       private: PlanInsert / PlanCreate / PlanUpdate / PlanDelete
#pragma once

#include "src/sql/ast.h"
#include "src/sql/logical_plan.h"
#include "src/sql/schema_manager.h"
#include <memory>
#include <string>

namespace raftsql {
// 将 AST 转换为初始逻辑计划（未优化）
class Planner {
public:
  // 【核心修改 1】构造函数强行注入 SchemaManager
  explicit Planner(SchemaManager &schema_manager)
      : schema_manager_(schema_manager), error_msg_("") {}

  // 提供获取错误信息的接口
  std::string GetErrorMsg() const { return error_msg_; }

  std::unique_ptr<LogicalPlan> Plan(const Stmt &stmt);
  std::unique_ptr<LogicalPlan> PlanSelect(const SelectStmt &stmt);

private:
  std::unique_ptr<LogicalPlan> PlanInsert(const InsertStmt &stmt);
  std::unique_ptr<LogicalPlan> PlanCreate(const CreateTableStmt &stmt);
  std::unique_ptr<LogicalPlan> PlanUpdate(const UpdateStmt &stmt);
  std::unique_ptr<LogicalPlan> PlanDelete(const DeleteStmt &stmt);

  SchemaManager &schema_manager_; // 【核心修改 2】常驻元数据图纸句柄
  std::string error_msg_;         // 暂存编译期错误信息
};
} // namespace raftsql