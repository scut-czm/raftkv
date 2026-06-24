#include "src/sql/operators/parallel_aggregate_operator.h"

#include <algorithm>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace raftsql {
// =========================================================================
// 构造函数：完美初始化所有多线程并行上下文与存储连接句柄
// =========================================================================
ParallelAggregateOperator::ParallelAggregateOperator(
    KvClientInterface *client, const std::string &table_name,
    const TableSchema &schema, const std::string &agg_func,
    const std::string &agg_column, int parallelism)
    : client_(client), table_name_(table_name), schema_(schema),
      agg_func_(agg_func), agg_column_(agg_column), parallelism_(parallelism),
      emitted_(false) {}

// =========================================================================
// Open()：大坝算子核心阻断点，多线程并行分片计算（Partial Agg 阶段）
// =========================================================================
void ParallelAggregateOperator::Open() {
  // 1. 调用编解码器，获取目标表在 RocksDB 中的全量物理区间闭包
  auto [start_key, end_key] = RowCodec::TableScanRange(table_name_);

  // 2. 暴力拉回全表原始 KV 字节对（内存分片计算准备）
  auto all_kvs = client_->Scan(start_key, end_key);
  if (all_kvs.empty()) {
    // 极端边界防御：如果是一张空表，COUNT 返回 0，其余聚合函数返回标准 NULL
    result_value_ = (agg_func_ == "COUNT") ? "0" : "NULL";
    emitted_ = false;
    return;
  }

  int64_t total = static_cast<int64_t>(all_kvs.size());
  // 3. 核心分片数学公式：按照行号均分给 parallelism_ 个工作线程
  int64_t chunk = (total + parallelism_ - 1) / parallelism_;

  //// 4. 轰鸣启动标准 C++11 std::async 多核并行计算矩阵（Partial Agg 阶段）
  std::vector<std::future<PartialAggResult>> futures;
  for (int w = 0; w < parallelism_; ++w) {
    int64_t from = w * chunk;
    int64_t to = std::min(from + chunk, total);
    if (from >= total) {
      break; // 算力配额已经覆盖全量行，终止多余线程拉起
    }

    futures.push_back(std::async(
        std::launch::async, [this, &all_kvs, from, to]() -> PartialAggResult {
          PartialAggResult partial;
          for (int64_t i = from; i < to; ++i) {
            // 物理反序列化解包：此步骤在各子线程内部并发对撞，直接稀释了 CPU
            // 瓶颈
            Row row = RowCodec::DecodeRow(all_kvs[i].second, schema_);
            // 符合 ANSI-SQL 标准的精确过滤分流
            if (agg_func_ == "COUNT" && agg_column_ == "*") {
              partial.count++;
            } else {
              auto it = row.find(agg_column_);
              if (it != row.end()) {
                if (agg_func_ == "COUNT") {
                  partial.count++; // 只有当该列非 NULL 时，COUNT(col) 才自增
                } else {
                  try {
                    int64_t val = std::stoll(it->second);
                    partial.sum += val;
                    if (!partial.min_val || val < *partial.min_val) {
                      partial.min_val = val;
                    }
                    if (!partial.max_val || val > *partial.max_val) {
                      partial.max_val = val;
                    }
                  } catch (...) {
                    // 脏数据物理沙箱：非数字列直接引发 stoll 崩溃时，安全跳过
                  }
                }
              }
            }
          }
          return partial;
        }));
  }
  // 5. 阻塞收集多路战果（对应分布式架构下的 Final Agg 聚合收口）
  std::vector<PartialAggResult> partials;
  for (auto &f : futures) {
    partials.push_back(f.get()); // 隐式执行 thread.join() 强步调同步
  }

  // 6. 终审合并
  auto merged = MergeResults(partials);

  // 7. 将最终融合的强类型标量转换为输出文本，更新全局状态
  if (agg_func_ == "COUNT") {
    result_value_ = std::to_string(merged.count);
  } else if (agg_func_ == "SUM") {
    result_value_ = std::to_string(merged.sum);
  } else if (agg_func_ == "MIN") {
    result_value_ = merged.min_val ? std::to_string(*merged.min_val) : "NULL";
  } else if (agg_func_ == "MAX") {
    result_value_ = merged.max_val ? std::to_string(*merged.max_val) : "NULL";
  }

  emitted_ = false; // 蓄势待发，等待上层 Next 拉动活塞
}

// =========================================================================
// Next()：流式单行输出控制，契合 Volcano 模型规范
// =========================================================================
bool ParallelAggregateOperator::Next(Row *row) {
  if (!row) {
    return false;
  }
  if (emitted_) {
    return false; // 第二次拉动？瞬间短路拉闸
  }
  emitted_ = true;

  row->clear();
  // 严格遵循标准关系代数合成键格式：FUNC(col) -> 如 "SUM(age)"
  std::string col_key = agg_func_ + "(" + agg_column_ + ")";
  (*row)[col_key] = result_value_;
  return true;
}
// =========================================================================
// Close()：上下文安全打扫战场
// =========================================================================
void ParallelAggregateOperator::Close() {
  result_value_.clear();
  emitted_ = false;
}

// =========================================================================
// MergeResults()：无锁局部战果终审汇聚算法（Final Agg 核心逻辑）
// =========================================================================
PartialAggResult ParallelAggregateOperator::MergeResults(
    const std::vector<PartialAggResult> &partials) {
  PartialAggResult result;
  for (const auto &p : partials) {
    result.count += p.count;
    result.sum += p.sum;
    // 极值合并的 std::optional 安全解包对撞
    if (p.min_val && (!result.min_val || *p.min_val < *result.min_val)) {
      result.min_val = p.min_val;
    }
    if (p.max_val && (!result.max_val || *p.max_val > *result.max_val)) {
      result.max_val = p.max_val;
    }
  }
  return result;
}

// =========================================================================
// WorkerScan()：头文件声明的私有防线填充，防止链接期缺失符号
// =========================================================================
PartialAggResult
ParallelAggregateOperator::WorkerScan(const std::string &start_key,
                                      const std::string &end_key) {
  // 架构演进占位：若未来将底层存储（RaftKV）扩展为真实的 Range
  // 物理分片协同扫描， 可将此函数作为具体的物理 RPC
  // 扫描执行体。目前暂由内存切片多线程方案平替。
  return PartialAggResult{};
}

} // namespace raftsql