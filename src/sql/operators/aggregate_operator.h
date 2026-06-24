// TODO: 声明 AggregateOperator : Operator
//       构造：child / agg_func / agg_column
//       private: result_value_ / emitted_

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "src/sql/operator.h"

namespace raftsql {
class AggregateOperator : public Operator {
public:
  AggregateOperator(std::unique_ptr<Operator> child, std::string agg_func,
                    std::string agg_column);

  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  std::unique_ptr<Operator> child_;
  std::string agg_func_;   // COUNT/SUM/MIN/MAX
  std::string agg_column_; // 列名或 "*"

  std::string result_value_;
  bool emitted_ = false;
};

} // namespace raftsql
