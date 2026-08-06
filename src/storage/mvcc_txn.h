#pragma once

#include <cstdint>
#include <google/protobuf/descriptor.pb.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "storage/mvcc_codec.h"
#include "storage/rocksdb_storage.h"

namespace raftkv {
// MVCC 操作的错误类型（对标 TiKV 的 KvError）。
struct MvccError {
  enum Kind {
    kNone = 0,
    kLocked, // 读/写被未提交锁阻塞 → 客户端 backoff / resolve
    kWriteConflict, // Prewrite 时发现更新的提交 → 事务重试
    kTxnNotFound, // Commit 时锁不在且无提交记录 → 事务已被回滚
    kAlreadyRolledBack, // 迟到的 Prewrite 撞上 rollback 墓碑
    kStorageError,      // 新增：RocksDB 写入失败
  };
  Kind kind = kNone;
  std::string key;
  std::string primary;
  uint64_t lock_ts = 0;
  uint64_t conflict_commit_ts = 0;
  std::string message;

  bool ok() const { return kind == kNone; }
  static MvccError Ok() { return MvccError{}; }
};

// CheckTxnStatus 的判定结果（对标 unistore mvcc.go:497 的四分支）。
struct TxnStatus {
  enum State {
    kCommitted, // primary 已提交 → 帮 secondary 提交
    kRolledBack, // primary 已回滚（或本次判定中被回滚）→ 回滚 secondary
    kLockAlive, // 锁未超 TTL → 返回剩余 TTL，调用方 backoff
  };

  State state = kLockAlive;
  uint64_t commit_ts = 0; // kCommitted 时有效
  uint64_t remaining_ttl_ms = 0;
};

// Percolator MVCC 原语。所有写方法都必须在 Raft on_apply 内调用
// （由 Raft 日志驱动，保证各副本对同一条日志产生完全相同的状态变更）；
// 读方法（Get/Scan/CheckTxnStatus 的只读部分）在 leader lease read 下
// 直接读状态机即可，不产生 Raft 日志。
//
// 三 CF 布局：
//   default: EncodeKey(key, start_ts)  -> 用户数据
//   lock:    key                       -> LockInfo
//   write:   EncodeKey(key, commit_ts) -> WriteInfo{kind, start_ts}
class MvccTxn {
public:
  explicit MvccTxn(RocksDbStorage *storage) : storage_(storage) {}

  // ---------- 读路径（快照隔离） ----------
  MvccError Get(const std::string &key, uint64_t snapshot_ts,
                std::optional<std::string> *value) const;

  MvccError Scan(const std::string &start_key, const std::string &end_key,
                 int limit, uint64_t snapshot_ts,
                 std::vector<std::pair<std::string, std::string>> *out) const;

  // ---------- 写路径（Percolator 两阶段） ----------
  MvccError Prewrite(const std::string &key, const std::string &value,
                     bool is_delete, const std::string &primary,
                     uint64_t start_ts, uint64_t ttl_ms);

  MvccError Commit(const std::string &key, uint64_t start_ts,
                   uint64_t commit_ts);

  MvccError Rollback(const std::string &key, uint64_t start_ts);

  // ---------- 残锁处理 ----------
  // 判定 primary 上事务的最终命运；now_ts 用于 TTL 判断。
  // 若 TTL 已过期会当场回滚 primary（写墓碑），因此也必须经 Raft apply。
  MvccError CheckTxnStatus(const std::string &primary, uint64_t lock_ts,
                           uint64_t now_ts, TxnStatus *status);

  // 根据 CheckTxnStatus 的结论推进 secondary：
  // commit_ts > 0 → 提交该 key；commit_ts == 0 → 回滚该 key。
  MvccError ResolveLock(const std::string &key, uint64_t start_ts,
                        uint64_t commit_ts);

private:
  // 找 key 在 write CF 中 commit_ts <= ts 的最新非 rollback 记录。
  std::optional<std::pair<uint64_t, WriteInfo>>
  SeekWrite(const std::string &key, uint64_t ts) const;

  // 找 key 是否存在 start_ts 恰好等于给定值的提交/回滚记录（幂等判断用）。
  std::optional<std::pair<uint64_t, WriteInfo>>
  GetTxnRecord(const std::string &key, uint64_t start_ts) const;

  RocksDbStorage *storage_;
};
} // namespace raftkv