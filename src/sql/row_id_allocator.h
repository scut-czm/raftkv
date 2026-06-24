// TODO: 声明 RowIdAllocator（TBase SCN 批量预分配思想）
//       kSegmentSize = 1000
//       Init(persisted_limit) / Allocate()
//       private: RefillSegment / current_(atomic) / limit_ / refill_mu_ /
//       raft_persist_fn_

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace raftsql {

// 预分配 row_id 段，每 kSegmentSize 次才走一次 Raft
// 灵感来源：TBase 借鉴 Oracle SCN 批量推进思想
class RowIdAllocator {
public:
  static constexpr int64_t kSegmentSize = 1000;

  explicit RowIdAllocator(std::function<bool(int64_t)> raft_persist_fn);

  // 从持久化存储恢复（启动时调用）
  void Init(int64_t persisted_limit);

  // 分配下一个 row_id（99% 情况为内存操作，无锁）
  int64_t Allocate();

private:
  void RefillSegment();

  std::atomic<int64_t> current_{0};
  int64_t limit_{0};
  std::mutex refill_mu_;
  std::function<bool(int64_t)> raft_persist_fn_;
};


} // namespace raftsql