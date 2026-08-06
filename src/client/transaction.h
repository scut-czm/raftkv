#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "client/kv_client.h"

namespace raftkv {
// 客户端乐观事务句柄（对标 TiDB client-go 的 KVTxn）。
//
// 生命周期：
//   Transaction txn(client);           // 取 start_ts（快照点）
//   txn.Get/Put/Delete(...);           // 读走快照，写进本地缓冲
//   txn.Commit(&err);                  // 2PC：Prewrite 全部 → Commit
//
// 读己之写（read-your-writes）：Get 先查本地写缓冲再走快照读。
// 冲突处理：Commit 返回 false + err，调用方整个事务重跑（乐观模型）。
class Transaction {
public:
  explicit Transaction(KVClient *client)
      : client_(client), start_ts_(client->GetTso(1)) {}

  ~Transaction() {
    if (!finished_) {
      Rollback(); // RAII：忘记 Commit 的事务自动回滚
    }
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  std::optional<std::string> Get(const std::string &key);
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key, int limit);

  void Put(const std::string &key, const std::string &value) {
    write_buffer_[key] = BufEntry{value, /*deleted=*/false};
  }
  void Delete(const std::string &key) {
    write_buffer_[key] = BufEntry{"", /*deleted=*/true};
  }

  // 两阶段提交。false 时 *err 说明原因（写冲突/事务被回滚等）。
  bool Commit(std::string *err);
  void Rollback();

  uint64_t start_ts() const { return start_ts_; }
  uint64_t commit_ts() const { return commit_ts_; } // Commit 成功后有效

private:
  struct BufEntry {
    std::string value;
    bool deleted = false;
  };

  // 快照读遇锁时的处理：CheckTxnStatus 判定 → resolve 或 backoff 重试。
  std::optional<std::string> SnapshotGetWithResolve(const std::string &key);

  // 撞锁后的统一决策：返回 true 表示应重试读，false 表示放弃。
  bool ResolveLock(const LockInfo &lock, int attempt);

  std::vector<std::string> BufferedKeys() const;

  KVClient *client_;
  uint64_t start_ts_;
  uint64_t commit_ts_ = 0;
  bool finished_ = false;
  static constexpr uint64_t kLockTtlMs = 3000;
  static constexpr int kMaxLockRetry = 8;
  // std::map 保证有序 → primary 恒为 begin()，事务重试时 primary 稳定。
  std::map<std::string, BufEntry> write_buffer_;
};
} // namespace raftkv
