// TODO: 声明 ProjectOperator : Operator
//       构造：child / columns（空=SELECT *）

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/sql/operator.h"

namespace raftsql {
class ProjectOperator : public Operator {
public:
  ProjectOperator(std::unique_ptr<Operator> child,
                  std::vector<std::string> columns);

  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  std::unique_ptr<Operator> child_;
  std::vector<std::string> columns_; // 空 = SELECT *
};

} // namespace raftsql