// TODO: Open() —— 遍历所有行，累积 count/sum/min/max，设置 result_value_
// TODO: Next() —— 只调用一次，返回 {"FUNC(col)": result_value_}
// TODO: 聚合键格式：agg_func_ + "(" + agg_column_ + ")"

#include "src/sql/operators/aggregate_operator.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace raftsql {

AggregateOperator::AggregateOperator(std::unique_ptr<Operator> child,
                                     std::string agg_func,
                                     std::string agg_column)
    : child_(std::move(child)), agg_func_(std::move(agg_func)),
      agg_column_(std::move(agg_column)) {}

bool AggregateOperator::Next(Row *row) {
  if (emitted_) {
    return false;
  }
  emitted_ = true;
  row->clear();
  std::string col_key = agg_func_ + "(" + agg_column_ + ")";
  (*row)[col_key] = result_value_;
  return true;
}

void AggregateOperator::Close() {}

void AggregateOperator::Open() {
  child_->Open();

  int64_t count = 0;
  int64_t sum = 0;
  std::optional<int64_t> min_val;
  std::optional<int64_t> max_val;

  Row row;
  while (child_->Next(&row)) {
    // 1. 如果是标准的 COUNT(*)，无条件累加行数
    if (agg_func_ == "COUNT" && agg_column_ == "*") {
      count++;

    } else {
      // 2. 如果是 COUNT(age) 或 SUM(age) 等，必须先去行里查该列存不存在（是否为
      // NULL）
      auto it = row.find(agg_column_);
      if (it != row.end()) {
        if (agg_func_ == "COUNT") {
          count++;
        } else {
          try {
            int64_t val = std::stoll(it->second);
            sum += val;
            if (!min_val || val < *min_val) {
              min_val = val;
            }
            if (!max_val || val > *max_val) {
              max_val = val;
            }
          } catch (...) {
            // 非数字列，SUM/MIN/MAX 跳过
          }
        }
      }
    }
    row.clear();
  }
  child_->Close();

  // 生成结果
  if (agg_func_ == "COUNT") {
    result_value_ = std::to_string(count);
  } else if (agg_func_ == "SUM") {
    result_value_ = std::to_string(sum);
  } else if (agg_func_ == "MIN") {
    result_value_ = min_val ? std::to_string(*min_val) : "NULL";
  } else if (agg_func_ == "MAX") {
    result_value_ = max_val ? std::to_string(*max_val) : "NULL";
  }
  emitted_ = false;
}
} // namespace raftsql