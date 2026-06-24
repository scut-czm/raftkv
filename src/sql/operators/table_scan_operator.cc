// TODO: Open() —— 计算 Scan 范围（谓词下推到 Key），调用 kv_client_->Scan
// TODO: Next() —— 逐行 RowCodec::DecodeRow，非主键谓词在此过滤
// TODO: Close() —— 清空 scan_results_ 和 cursor_
// TODO: CanNarrowScanRange —— 主键等值/范围条件可缩小范围
// TODO: NarrowScanRange —— =/>/>=/< /<=各种操作符转 Key 范围
// TODO: EvalPredicate / CompareValues —— 类型感知比较（int stoll，否则字符串）

#include "src/sql/operators/table_scan_operator.h"

#include <stdexcept>
#include <string>

namespace raftsql {
TableScanOperator::TableScanOperator(KvClientInterface *kv_client,
                                     std::string table_name, TableSchema schema,
                                     const Expr *scan_predicate)
    : kv_client_(kv_client), table_name_(std::move(table_name)),
      schema_(std::move(schema)), scan_predicate_(scan_predicate) {}

void TableScanOperator::Open() {
  auto [start_key, end_key] = RowCodec::TableScanRange(table_name_);

  // 谓词下推到 Key 范围（仅主键等值/范围）
  if (scan_predicate_ && CanNarrowScanRange(scan_predicate_)) {
    auto [ns, ne] = NarrowScanRange(scan_predicate_);
    start_key = ns;
    end_key = ne;
  }
  scan_results_ = kv_client_->Scan(start_key, end_key);
  cursor_ = 0;
}

void TableScanOperator::Close() {
  scan_results_.clear();
  cursor_ = 0;
}

bool TableScanOperator::Next(Row *row) {
  while (cursor_ < scan_results_.size()) {
    auto &[key, value] = scan_results_[cursor_++];
    *row = RowCodec::DecodeRow(value, schema_);

    // 如果谓词无法下推（非主键条件），在此过滤
    if (scan_predicate_ && !CanNarrowScanRange(scan_predicate_)) {
      if (!EvalPredicate(scan_predicate_, *row)) {
        continue;
      }
    }
    return true;
  }
  return false;
}

bool TableScanOperator::CanNarrowScanRange(const Expr *expr) const {
  if (!expr || expr->kind != Expr::Kind::kBinOp) {
    return false;
  }
  if (!expr->left || expr->left->kind != Expr::Kind::kColumn) {
    return false;
  }
  if (!expr->right || expr->right->kind != Expr::Kind::kLiteral) {
    return false;
  }
  // 检查左侧列是否是主键
  auto pk_col = RowCodec::GetPrimaryKeyColumn(schema_);
  return !pk_col.empty() && expr->left->col_name == pk_col;
}

std::pair<std::string, std::string>
TableScanOperator::NarrowScanRange(const Expr *expr) const {
  const std::string &pk_value = expr->right->literal;
  try {
    int64_t pk_id = std::stoll(pk_value);
    if (expr->op == "=") {
      auto key = RowCodec::EncodeRowKey(table_name_, pk_id);
      return {key, key + '\0'};
    }
    if (expr->op == ">") {
      auto key = RowCodec::EncodeRowKey(table_name_, pk_id + 1);
      return {key, table_name_ + ";"};
    }
    if (expr->op == ">=") {
      auto key = RowCodec::EncodeRowKey(table_name_, pk_id);
      return {key, table_name_ + ";"};
    }
    if (expr->op == "<") {
      auto key = RowCodec::EncodeRowKey(table_name_, pk_id);
      return {table_name_ + ":", key};
    }
    if (expr->op == "<=") {
      auto key = RowCodec::EncodeRowKey(table_name_, pk_id + 1);
      return {table_name_ + ":", key};
    }
  } catch (...) {
  }
  return RowCodec::TableScanRange(table_name_);
}

bool TableScanOperator::EvalPredicate(const Expr *expr, const Row &row) const {
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

std::string TableScanOperator::GetExprValue(const Expr *expr,
                                            const Row &row) const {
  if (expr->kind == Expr::Kind::kColumn) {
    auto it = row.find(expr->col_name);
    return (it != row.end()) ? it->second : "";
  }
  return expr->literal;
}

bool TableScanOperator::CompareValues(const std::string &left,
                                      const std::string &op,
                                      const std::string &right) const {
  try {
    int64_t l = std::stoll(left);
    int64_t r = std::stoll(right);
    if (op == "=") {
      return l == r;
    }
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