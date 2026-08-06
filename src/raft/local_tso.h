#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace raftkv {
// 单 Group 场景的本地单调时间戳分配器（Leader 上运行）。
// 编码与 TiDB TSO 同构：物理毫秒 << 18 | 逻辑计数。
// 好处：M6 换成全局 PD TSO 时，ts 的比较语义和存储格式完全不变。
//
// 正确性要点（面试必问）：
// 1. 单调性：CAS 循环保证并发下严格递增；
// 2. Leader 切换不回退：last_ts 定期（见 KVStateMachine 中每 kPersistBatch
//    个 ts 提交一条 Raft 日志持久化上界），新 Leader 用
//    RecoverTo(持久化上界) 初始化，保证绝不发出旧号。
class LocalTso {
public:
  static constexpr int kLogicalBits = 18;

  uint64_t Current() const { return last_.load(std::memory_order_acquire); }
  void RecoverTo(uint64_t ts) {
    uint64_t last = last_.load(std::memory_order_relaxed);
    while (last < ts &&
           !last_.compare_exchange_weak(last, ts, std::memory_order_acq_rel)) {
    }
  }

  uint64_t Next() {
    uint64_t phys = NowMs() << kLogicalBits;
    uint64_t last = last_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = std::max(phys, last + 1);
    } while (
        !last_.compare_exchange_weak(last, next, std::memory_order_acq_rel));
    return next;
  }

  // 批量取号：一次 CAS 原子跳 count，返回区间起点，区间为 [start, start+count)。
  // next >= last + count 保证 start = next - count + 1 > last，与已发号不重叠。
  uint64_t NextBatch(uint32_t count) {
    uint64_t phys = NowMs() << kLogicalBits;
    uint64_t last = last_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = std::max(phys, last + count);
    } while (
        !last_.compare_exchange_weak(last, next, std::memory_order_acq_rel));
    return next - count + 1;
  }

  // 受预留上界约束的批量取号：只在 (last, bound] 内发号。
  // 若发号会越过 bound（含物理时钟已冲过 bound 的情况）返回 false，
  // 调用方需先持久化更大的预留上界再重试；检查与 CAS 在同一循环内，
  // 并发下不会发出任何超出已持久化上界的号。
  bool NextBatchBounded(uint32_t count, uint64_t bound, uint64_t *start) {
    uint64_t phys = NowMs() << kLogicalBits;
    uint64_t last = last_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = std::max(phys, last + count);
      if (next > bound) {
        return false;
      }
    } while (
        !last_.compare_exchange_weak(last, next, std::memory_order_acq_rel));
    *start = next - count + 1;
    return true;
  }

private:
  static uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  std::atomic<uint64_t> last_{0};
};

} // namespace raftkv