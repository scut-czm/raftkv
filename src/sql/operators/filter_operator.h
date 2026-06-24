// TODO: 声明 FilterOperator : Operator
//       构造：child / predicate
//       private: EvalPredicate / GetExprValue / CompareValues

#pragma once

#include <memory>
#include <string>

#include "src/sql/ast.h"
#include "src/sql/operator.h"

namespace raftsql {
class FilterOperator : public Operator {
public:
  FilterOperator(std::unique_ptr<Operator> child, const Expr *predicate);
  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  bool EvalPredicate(const Expr *expr, const Row &row) const;
  std::string GetExprValue(const Expr *epxr, const Row &row) const;
  bool CompareValues(const std::string &left, const std::string &op,
                     const std::string &right) const;
  std::unique_ptr<Operator> child_;
  const Expr *predicate_;
};
} // namespace raftsql