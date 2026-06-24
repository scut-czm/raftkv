// TODO: Open() —— child_->Open() + count_ = 0
// TODO: Next() —— count_ >= limit_ 返回 false，否则转发 child_->Next
#include "src/sql/operators/limit_operator.h"

namespace raftsql {

LimitOperator::LimitOperator(std::unique_ptr<Operator> child, int64_t limit)
    : child_(std::move(child)), limit_(limit) {}

void LimitOperator::Open() {
  child_->Open();
  count_ = 0;
}

void LimitOperator::Close() { child_->Close(); }

bool LimitOperator::Next(Row *row) {
  if (count_ >= limit_) {
    return false;
  }
  if (!child_->Next(row)) {
    return false;
  }
  count_++;
  return true;
}
} // namespace raftsql