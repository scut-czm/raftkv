// TODO: 声明 LimitOperator : Operator
//       构造：child / limit
//       private: count_
#pragma once

#include <cstdint>
#include <memory>

#include "src/sql/operator.h"

namespace raftsql {

class LimitOperator : public Operator {
public:
  LimitOperator(std::unique_ptr<Operator> child, int64_t limit);

  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  std::unique_ptr<Operator> child_;
  int64_t limit_;
  int64_t count_;
};
} // namespace raftsql