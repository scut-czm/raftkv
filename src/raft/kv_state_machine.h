#pragma once

#include "raft/local_tso.h"
#include "storage/mvcc_txn.h"
#include "storage/rocksdb_storage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <braft/raft.h>
#include <braft/storage.h>
#include <braft/util.h>
#include <butil/status.h>

namespace raftkv {
// KVStateMachine：基于 RocksDB 的 braft 状态机
// 替换原版 braft_learning 中的 std::map 存储
class KVStateMachine : public braft::StateMachine {
public:
  explicit KVStateMachine(std::shared_ptr<RocksDbStorage> storage);
  ~KVStateMachine() = default;

  // ── braft::StateMachine 接口 ──
  void on_apply(braft::Iterator &iter) override;
  void on_snapshot_save(braft::SnapshotWriter *writer,
                        braft::Closure *done) override;
  int on_snapshot_load(braft::SnapshotReader *reader) override;
  void on_leader_start(int64_t term) override;
  void on_leader_stop(const butil::Status &status) override;

  // ── 业务接口（直接读状态机，不走 Raft）──
  // 旧非事务读：读取 data CF。
  bool Get(const std::string &key, std::string *value) const;

  // MVCC 快照读：读取 lock/write/default CF。
  MvccError TxnGet(const std::string &key, uint64_t snapshot_ts,
                   std::optional<std::string> *value) const;

  bool IsLeader() const { return is_leader_.load(std::memory_order_acquire); }
  int64_t LeaderTerm() const {
    return leader_term_.load(std::memory_order_acquire);
  }

  int64_t LastAppliedIndex() const {
    return last_applied_index_.load(std::memory_order_acquire);
  }

  // 等待 last_applied_index >= target，超时返回 false
  bool WaitApplied(int64_t target, int64_t timeout_ms) const;

  // ── 本地 TSO（仅 leader 发号）──
  // 预留式发号：先通过 OP_TSO_RESERVE 日志持久化「未来上界」，
  // 只允许发号到该上界；leader 切换时 RecoverTo(上界)，绝不重复发号。
  LocalTso &Tso() { return tso_; }
  uint64_t TsoReserved() const {
    return tso_reserved_.load(std::memory_order_acquire);
  }
  // 每次预留覆盖的物理时间窗口（TSO 低 18 位为逻辑位，1ms = 1<<18）。
  // 必须远大于一条 Raft 日志的提交延迟：开 raft_sync 时 fsync 可达数十
  // 毫秒，若预留窗口比这还小，日志 apply 完墙钟已越过上界，重试必然耗尽。
  static constexpr uint64_t kTsoReserveAheadMs = 3000;
  static constexpr uint64_t kTsoReserveAhead = kTsoReserveAheadMs << 18;

  // 旧非事务范围读：读取 data CF。
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

  // MVCC 快照范围读：读取 write/default/lock CF。
  MvccError
  TxnScan(const std::string &start_key, const std::string &end_key, int limit,
          uint64_t snapshot_ts,
          std::vector<std::pair<std::string, std::string>> *out) const;

private:
  // 从 meta CF 读回已持久化的 TSO 预留上界（启动 / 装完快照后调用）
  void LoadTsoReserved();

  std::shared_ptr<RocksDbStorage> storage_;
  MvccTxn mvcc_;
  LocalTso tso_;
  std::atomic<uint64_t> tso_reserved_{0};
  std::atomic<bool> is_leader_{false};
  std::atomic<int64_t> leader_term_{-1};
  std::atomic<int64_t> last_applied_index_{0};
  mutable std::mutex applied_mutex_;
  mutable std::condition_variable applied_cv_;
};
} // namespace raftkv