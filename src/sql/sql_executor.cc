// TODO: 构造函数 —— 初始化 schema_manager_ / row_id_allocator_，Init(1)
// TODO: Execute() —— 解析 SQL，std::visit 分派语句类型
// TODO: ExecuteCreate —— TableSchema proto 构造 + SchemaManager::CreateTable
// TODO: ExecuteInsert —— 校验列/值，用主键值作 row_id，RowCodec::EncodeRow +
// kv_client_->Put
// TODO: ExecuteSelect —— 聚合路径（Scan+计算）/
// 普通路径（Planner→Optimizer→PhysicalPlanner→Execute）
// TODO: ExecuteUpdate —— 全表 Scan → WHERE 匹配 → 修改字段 → EncodeRow + Put
// TODO: ExecuteDelete —— 全表 Scan → WHERE 匹配 → Delete
// TODO: EvalPredicate / GetExprValue / CompareValues

#include "src/sql/sql_executor.h"

#include <stdexcept>
#include <string>
#include <variant>

#include "src/sql/lexer.h"
#include "src/sql/logical_optimizer.h"
#include "src/sql/parser.h"
#include "src/sql/physical_planner.h"
#include "src/sql/planner.h"

namespace raftsql {
SQLExecutor::SQLExecutor(KvClientInterface *kv_client)
    : kv_client_(kv_client), schema_manager_(kv_client),
      row_id_allocator_([this](int64_t new_limit) -> bool { return true; }) {
  row_id_allocator_.Init(1); // RowIdAllocator 初始化（启动时从第 1 行开始）
}

SQLExecutor::QueryResult SQLExecutor::Execute(const std::string &sql) {
  Parser parser(sql);
  auto stmt_opt = parser.Parse();
  if (!stmt_opt) {
    return {false, "Parse error: " + parser.GetError()};
  }

  return std::visit(
      [this](const auto &stmt) -> QueryResult {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, CreateTableStmt>) {
          return ExecuteCreate(stmt);
        } else if constexpr (std::is_same_v<T, InsertStmt>) {
          return ExecuteInsert(stmt);
        } else if constexpr (std::is_same_v<T, SelectStmt>) {
          return ExecuteSelect(stmt);
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
          return ExecuteUpdate(stmt);
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
          return ExecuteDelete(stmt);
        }
        return {false, "Unknown statement type"};
      },
      *stmt_opt);
}

SQLExecutor::QueryResult
SQLExecutor::ExecuteCreate(const CreateTableStmt &stmt) {
  TableSchema proto_schema;
  proto_schema.set_table_name(stmt.table_name);

  for (const auto &col : stmt.columns) {
    auto *col_def = proto_schema.add_columns();
    col_def->set_name(col.name);
    std::string type_upper = col.type;
    for (auto &c : type_upper) {
      c = static_cast<char>(toupper(c));
    }
    if (type_upper == "INT" || type_upper == "INTEGER") {
      col_def->set_type(DT_INT);
    } else if (type_upper.find("VARCHAR") != std::string::npos) {
      col_def->set_type(DT_VARCHAR);
      auto pos1 = col.type.find('(');
      auto pos2 = col.type.find(')');
      if (pos1 != std::string::npos && pos2 != std::string::npos) {
        col_def->set_varchar_len(
            std::stoi(col.type.substr(pos1 + 1, pos2 - pos1 - 1)));
      }
    } else if (type_upper.find("FLOAT") != std::string::npos ||
               type_upper.find("DOUBLE") != std::string::npos) {
      col_def->set_type(DT_FLOAT);
    } else if (type_upper == "BOOL" || type_upper == "BOOLEAN") {
      col_def->set_type(DT_BOOL);
    } else {
      col_def->set_type(DT_VARCHAR);
    }
    col_def->set_primary_key(col.primary_key);
  }
  if (!schema_manager_.CreateTable(proto_schema)) {
    return {false, "Table '" + stmt.table_name + "' already exists"};
  }
  return {true, "", {}, 0};
}

SQLExecutor::QueryResult SQLExecutor::ExecuteInsert(const InsertStmt &stmt) {
  auto schema_opt = schema_manager_.GetSchema(stmt.table_name);
  if (!schema_opt) {
    return {false, "Table '" + stmt.table_name + "' does not exist"};
  }
  auto schema = *schema_opt;
  if (stmt.columns.size() != stmt.values.size()) {
    return {false, "Column count (" + std::to_string(stmt.columns.size()) +
                       ") != value count (" +
                       std::to_string(stmt.values.size()) + ")"};
  }
  for (const auto &col : stmt.columns) {
    bool found = false;
    for (const auto &col_def : schema.columns()) {
      if (col_def.name() == col) {
        found = true;
        break;
      }
    }
    if (!found) {
      return {false, "Unknown column '" + col + "'"};
    }
  }
  Row row;
  for (size_t i = 0; i < stmt.columns.size(); ++i) {
    row[stmt.columns[i]] = stmt.values[i];
  }
  // 优先用主键列值作为 row_id，确保 WHERE pk=N 的谓词下推正确
  int64_t row_id = row_id_allocator_.Allocate();
  const std::string pk_col = RowCodec::GetPrimaryKeyColumn(schema);
  if (!pk_col.empty()) {
    auto pk_it = row.find(pk_col);
    if (pk_it != row.end()) {
      try {
        row_id = std::stoll(pk_it->second);
      } catch (...) {
      }
    }
  }
  auto key = RowCodec::EncodeRowKey(stmt.table_name, row_id);
  auto value = RowCodec::EncodeRow(row, schema);

  // =========================================================================
  // 🛡️ 【核心修改点】主键唯一性约束刚性布防 (Read-Before-Write)
  // =========================================================================
  // 利用多态接口反向嗅探，只要 Get 返回的值不是空串，说明底层 RocksDB
  // 已有同主键老数据

  if (!kv_client_->Get(key).empty()) {
    std::string pk_val = "unknown";
    if (!pk_col.empty() && row.find(pk_col) != row.end()) {
      pk_val = row[pk_col];
    }
    return {false, "Duplicate entry '" + pk_val + "' for key 'PRIMARY'", {}, 0};
  }
  // =========================================================================
  // 🚀 防线通过，执行物理分布式共识写入，并同步对 Put 返回值进行严格安全审计
  // =========================================================================
  if (!kv_client_->Put(key, value)) {
    return {false,
            "Storage engine layer write rejected or partition broken",
            {},
            0};
  }
  return {true, "", {}, 1};
}

SQLExecutor::QueryResult SQLExecutor::ExecuteSelect(const SelectStmt &stmt) {
  // 1. 获取 Schema
  auto schema_opt = schema_manager_.GetSchema(stmt.table_name);
  if (!schema_opt) {
    return {false, "Table '" + stmt.table_name + "' does not exist"};
  }
  // // 聚合查询路径
  // if (!stmt.agg_func.empty()) {
  //   auto [start_key, end_key] = RowCodec::TableScanRange(stmt.table_name);
  //   auto scan_result = kv_client_->Scan(start_key, end_key);

  //   int64_t count = 0;
  //   int64_t sum = 0;
  //   std::optional<int64_t> min_val, max_val;

  //   for (const auto &[key, value] : scan_result) {
  //     Row row = RowCodec::DecodeRow(value, *schema_opt);
  //     if (stmt.where_expr && !EvalPredicate(stmt.where_expr.get(), row)) {
  //       continue;
  //     }
  //     count++;
  //     if (stmt.agg_func != "COUNT" || stmt.agg_column != "*") {
  //       auto it = row.find(stmt.agg_column);
  //       if (it != row.end()) {
  //         try {
  //           int64_t val = std::stoll(it->second);
  //           sum += val;
  //           if (!min_val || val < *min_val) {
  //             min_val = val;
  //           }
  //           if (!max_val || val > *max_val) {
  //             max_val = val;
  //           }
  //         } catch (...) {
  //         }
  //       }
  //     }
  //   }
  //   std::string result_val;
  //   std::string col_key = stmt.agg_func + "(" + stmt.agg_column + ")";
  //   if (stmt.agg_func == "COUNT") {
  //     result_val = std::to_string(count);
  //   } else if (stmt.agg_func == "SUM") {
  //     result_val = std::to_string(sum);
  //   } else if (stmt.agg_func == "MIN") {
  //     result_val = min_val ? std::to_string(*min_val) : "NULL";
  //   } else if (stmt.agg_func == "MAX") {
  //     result_val = max_val ? std::to_string(*max_val) : "NULL";
  //   }

  //   Row result_row;
  //   result_row[col_key] = result_val;
  //   return {true, "", {result_row}, 0};
  // }
  // 普通 SELECT：Planner → Optimizer → PhysicalPlanner → Execute
  // 2. 构建逻辑计划：Scan → Filter → Project
  Planner planner(schema_manager_);
  auto logical_plan = planner.PlanSelect(stmt);
  if (!logical_plan) {
    // 规划期失败，直接拦截并上抛错误，拒绝进入物理执行算子层
    return {false, planner.GetErrorMsg()};
  }

  // 3. 优化（谓词下推 + 列裁剪 + 常量折叠）
  LogicalOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(logical_plan));

  // 4. 物理计划：LogicalPlan → Operator 树
  PhysicalPlanner physical_planner(
      kv_client_, [this](const std::string &t) -> std::optional<TableSchema> {
        return schema_manager_.GetSchema(t);
      });

  auto root_op = physical_planner.Plan(optimized.get());

  if (!root_op) {
    return {false, "Failed to plan query"};
  }

  // 5. 执行
  root_op->Open();
  QueryResult result{true};
  Row row;
  while (root_op->Next(&row)) {
    result.rows.push_back(std::move(row));
    row.clear();
  }
  root_op->Close();
  return result;
}
SQLExecutor::QueryResult SQLExecutor::ExecuteUpdate(const UpdateStmt &stmt) {

  // 1. 获取 Schema
  auto schema_opt = schema_manager_.GetSchema(stmt.table_name);
  if (!schema_opt) {
    return {false, "Table '" + stmt.table_name + "' does not exist"};
  }
  const auto &schema = *schema_opt;

  // 2. 全表 Scan 获取所有行
  auto [start_key, end_key] = RowCodec::TableScanRange(stmt.table_name);
  auto scan_result = kv_client_->Scan(start_key, end_key);
  int64_t affected = 0;

  // 3. 逐行判断 WHERE 条件
  for (const auto &[key, value] : scan_result) {
    Row row = RowCodec::DecodeRow(value, schema);

    // 评估 WHERE 条件
    if (stmt.where_expr && !EvalPredicate(stmt.where_expr.get(), row)) {
      continue;
    }

    // 4. 修改匹配行的字段
    for (const auto &[col_name, new_val] : stmt.set_clauses) {
      row[col_name] = new_val;
    }

    // 5. 重新编码并 Put 回写（原 key 不变）
    auto new_value = RowCodec::EncodeRow(row, schema);
    kv_client_->Put(key, new_value);
    ++affected;
  }
  return {true, "", {}, affected};
}

SQLExecutor::QueryResult SQLExecutor::ExecuteDelete(const DeleteStmt &stmt) {

  // 1. 获取 Schema
  auto schema_opt = schema_manager_.GetSchema(stmt.table_name);
  if (!schema_opt) {
    return {false, "Table '" + stmt.table_name + "' does not exist"};
  }
  const auto &schema = *schema_opt;

  // 2. 全表 Scan
  auto [start_key, end_key] = RowCodec::TableScanRange(stmt.table_name);
  auto scan_results = kv_client_->Scan(start_key, end_key);

  int64_t affected = 0;
  // 3. 逐行判断 WHERE 条件
  for (const auto &[key, value] : scan_results) {
    Row row = RowCodec::DecodeRow(value, schema);

    if (stmt.where_expr && !EvalPredicate(stmt.where_expr.get(), row)) {
      continue;
    }

    // 4. 删除匹配行
    kv_client_->Delete(key);
    ++affected;
  }
  return {true, "", {}, affected};
}

bool SQLExecutor::EvalPredicate(const Expr *expr, const Row &row) const {
  if (!expr) {
    return true;
  }
  switch (expr->kind) {
  case Expr::Kind::kColumn:
    return row.count(expr->col_name) > 0 && !row.at(expr->col_name).empty();

  case Expr::Kind::kLiteral:
    return !expr->literal.empty();

  case Expr::Kind::kBinOp: {
    if (expr->op == "AND") {
      return EvalPredicate(expr->left.get(), row) &&
             EvalPredicate(expr->right.get(), row);
    }
    if (expr->op == "OR") {
      return EvalPredicate(expr->left.get(), row) ||
             EvalPredicate(expr->right.get(), row);
    }
    return CompareValues(GetExprValue(expr->left.get(), row), expr->op,
                         GetExprValue(expr->right.get(), row));
  }
  }
  return false;
}

std::string SQLExecutor::GetExprValue(const Expr *expr, const Row &row) const {
  if (expr->kind == Expr::Kind::kColumn) {
    auto it = row.find(expr->col_name);
    return (it != row.end()) ? it->second : "";
  }
  return expr->literal;
}

bool SQLExecutor::CompareValues(const std::string &left, const std::string &op,
                                const std::string &right) const {
  try {
    int64_t l = std::stoll(left);
    int64_t r = std::stoll(right);
    if (op == "=")
      return l == r;
    if (op == "!=")
      return l != r;
    if (op == "<")
      return l < r;
    if (op == ">")
      return l > r;
    if (op == "<=")
      return l <= r;
    if (op == ">=")
      return l >= r;
  } catch (...) {
    if (op == "=")
      return left == right;
    if (op == "!=")
      return left != right;
    if (op == "<")
      return left < right;
    if (op == ">")
      return left > right;
    if (op == "<=")
      return left <= right;
    if (op == ">=")
      return left >= right;
  }
  return false;
}
} // namespace raftsql