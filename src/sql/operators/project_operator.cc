// TODO: Open() —— child_->Open()
// TODO: Next() —— columns_为空则全行，否则只保留指定列

#include "src/sql/operators/project_operator.h"

namespace raftsql {

ProjectOperator::ProjectOperator(std::unique_ptr<Operator> child,
                                 std::vector<std::string> columns)
    : child_(std::move(child)), columns_(std::move(columns)) {}

void ProjectOperator::Open() { child_->Open(); }

void ProjectOperator::Close() { child_->Close(); }

bool ProjectOperator::Next(Row *row) {
  Row full_row;
  // 1. 递归向下层（如 Filter 或 Join）索要一整行完整数据
  if (!child_->Next(&full_row)) {
    return false;
  }
  // 2. 优化路径：如果是 SELECT * (columns_ 为空)
  if (columns_.empty()) {
    *row = std::move(full_row); // 极好！利用移动语义消灭了深拷贝
  } else {
    // 3. 核心列裁剪路径
    row->clear();
    for (const auto &col : columns_) {
      auto it = full_row.find(col);
      if (it != full_row.end()) {
        (*row)[col] = it->second; // 像筛子一样，只把需要的列偷渡到上层
      }
    }
  }
  return true;
}
} // namespace raftsql