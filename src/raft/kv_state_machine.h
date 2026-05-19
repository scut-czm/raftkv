#pragma once

#include "storage/rocksdb_storage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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
  bool Get(const std::string &key, std::string *value) const;
  bool IsLeader() const { return is_leader_.load(std::memory_order_acquire); }
  int64_t LeaderTerm() const {
    return leader_term_.load(std::memory_order_acquire);
  }

  int64_t LastAppliedIndex() const {
    return last_applied_index_.load(std::memory_order_acquire);
  }

  // 等待 last_applied_index >= target，超时返回 false
  bool WaitApplied(int64_t target, int64_t timeout_ms) const;

  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

private:
  std::shared_ptr<RocksDbStorage> storage_;
  std::atomic<bool> is_leader_{false};
  std::atomic<int64_t> leader_term_{-1};
  std::atomic<int64_t> last_applied_index_{0};
  mutable std::mutex applied_mutex_;
  mutable std::condition_variable applied_cv_;
};
} // namespace raftkv