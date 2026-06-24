// TODO: 定义 Operator 基类（Volcano 火山模型）
//       virtual void Open() = 0
//       virtual bool Next(Row* row) = 0
//       virtual void Close() = 0

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/sql/table_schema.h"

namespace raftsql {
// 火山模型基类（对标 BusTub AbstractExecutor）
class Operator {
public:
  virtual ~Operator() = default;

  // 初始化（分配资源、首次 Scan 等）
  virtual void Open() = 0;

  // 获取下一行，返回 false 表示结束
  virtual bool Next(Row *row) = 0;

  // 释放资源
  virtual void Close() = 0;
};
} // namespace raftsql