// TODO: Plan() —— switch(plan->type) 分派到各 Plan* 方法
// TODO: PlanScan() —— schema_provider_(table_name) + TableScanOperator
// TODO: PlanFilter / PlanProject / PlanAggregate / PlanLimit —— 递归 Plan()
// 子节点

#include "src/sql/physical_planner.h"

#include "src/sql/operators/aggregate_operator.h"
#include "src/sql/operators/filter_operator.h"
#include "src/sql/operators/limit_operator.h"
#include "src/sql/operators/project_operator.h"
#include "src/sql/operators/table_scan_operator.h"

namespace raftsql {
PhysicalPlanner::PhysicalPlanner(KvClientInterface *kv_client,
                                 SchemaProvider schema_provider)
    : kv_client_(kv_client), schema_provider_(std::move(schema_provider)) {}

std::unique_ptr<Operator> PhysicalPlanner::Plan(const LogicalPlan *plan) {
  if (!plan) {
    return nullptr;
  }
  switch (plan->type) {
  case PlanType::kScan:
    return PlanScan(plan);
  case PlanType::kFilter:
    return PlanFilter(plan);
  case PlanType::kProject:
    return PlanProject(plan);
  case PlanType::kAggregate:
    return PlanAggregate(plan);
  case PlanType::kLimit:
    return PlanLimit(plan);
  default:
    return nullptr;
  }
}
std::unique_ptr<Operator> PhysicalPlanner::PlanScan(const LogicalPlan *plan) {
  auto schema_opt = schema_provider_(plan->table_name);
  if (!schema_opt) {
    return nullptr;
  }
  return std::make_unique<TableScanOperator>(
      kv_client_, plan->table_name, *schema_opt, plan->scan_predicate.get());
}

std::unique_ptr<Operator> PhysicalPlanner::PlanFilter(const LogicalPlan *plan) {
  if (plan->children.empty()) {
    return nullptr;
  }
  auto child = Plan(plan->children[0].get());
  if (!child) {
    return nullptr;
  }
  return std::make_unique<FilterOperator>(std::move(child),
                                          plan->filter_predicate.get());
}

std::unique_ptr<Operator>
PhysicalPlanner::PlanProject(const LogicalPlan *plan) {
  if (plan->children.empty())
    return nullptr;
  auto child = Plan(plan->children[0].get());
  if (!child)
    return nullptr;
  return std::make_unique<ProjectOperator>(std::move(child),
                                           plan->project_columns);
}

std::unique_ptr<Operator>
PhysicalPlanner::PlanAggregate(const LogicalPlan *plan) {
  if (plan->children.empty())
    return nullptr;
  auto child = Plan(plan->children[0].get());
  if (!child)
    return nullptr;
  return std::make_unique<AggregateOperator>(std::move(child), plan->agg_func,
                                             plan->agg_column);
}

std::unique_ptr<Operator> PhysicalPlanner::PlanLimit(const LogicalPlan *plan) {
  if (plan->children.empty())
    return nullptr;
  auto child = Plan(plan->children[0].get());
  if (!child)
    return nullptr;
  return std::make_unique<LimitOperator>(std::move(child), plan->limit_count);
}

} // namespace raftsql