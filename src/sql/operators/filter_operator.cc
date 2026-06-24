// TODO: Open() —— child_->Open()
// TODO: Next() —— 循环 child_->Next，EvalPredicate 过滤
// TODO: EvalPredicate / GetExprValue / CompareValues

#include "src/sql/operators/filter_operator.h"

#include <stdexcept>
#include <string>

namespace raftsql {
FilterOperator::FilterOperator(std::unique_ptr<Operator> child,
                               const Expr *predicate)
    : child_(std::move(child)), predicate_(predicate) {}

void FilterOperator::Open() { child_->Open(); }

bool FilterOperator::Next(Row *row) {
  // 驱动子算子（如 TableScan）持续向上抽水
  while (child_->Next(row)) {
    if (EvalPredicate(predicate_, *row)) {
      return true; // 撞见真值行，直接拦截并向上层缴械，不再引发 clear
    }
    row->clear(); // 质检失败：利用安全的点运算符清空当前行的哈希槽位，准备迎接下一行的覆写
  }
  return false; // 下层水源彻底干涸
}

void FilterOperator::Close() {
  if (child_) {
    child_->Close();
  }
}

bool FilterOperator::EvalPredicate(const Expr *expr, const Row &row) const {
  if (!expr)
    return true;
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

std::string FilterOperator::GetExprValue(const Expr *expr,
                                         const Row &row) const {
  if (expr->kind == Expr::Kind::kColumn) {
    auto it = row.find(expr->col_name);
    return (it != row.end()) ? it->second : "";
  }
  return expr->literal;
}

bool FilterOperator::CompareValues(const std::string &left,
                                   const std::string &op,
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
