// TODO: 定义 SchemaProvider 类型别名 function<optional<TableSchema>(string)>
// TODO: 声明 PhysicalPlanner 类
//       public:  Plan(LogicalPlan*) -> unique_ptr<Operator>
//       private: PlanScan / PlanFilter / PlanProject / PlanAggregate /
//       PlanLimit
//                kv_client_ / schema_provider_

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "schema.pb.h"
#include "src/sql/kv_client_interface.h"
#include "src/sql/logical_plan.h"
#include "src/sql/operator.h"

namespace raftsql {
// Schema 提供者函数类型
using SchemaProvider =
    std::function<std::optional<TableSchema>(const std::string &)>;

// 将 LogicalPlan 转换为 Operator 树（Volcano 模型）
class PhysicalPlanner {
public:
  PhysicalPlanner(KvClientInterface *kv_client, SchemaProvider scheme_provider);

  std::unique_ptr<Operator> Plan(const LogicalPlan *plan);

private:
  std::unique_ptr<Operator> PlanScan(const LogicalPlan *plan);
  std::unique_ptr<Operator> PlanFilter(const LogicalPlan *plan);
  std::unique_ptr<Operator> PlanProject(const LogicalPlan *plan);
  std::unique_ptr<Operator> PlanAggregate(const LogicalPlan *plan);
  std::unique_ptr<Operator> PlanLimit(const LogicalPlan *plan);

  KvClientInterface *kv_client_;
  SchemaProvider schema_provider_;
};

} // namespace raftsql
