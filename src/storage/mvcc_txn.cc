#include "storage/mvcc_txn.h"
#include "storage/mvcc_codec.h"

#include <cassert>
#include <cstdint>
#include <rocksdb/slice.h>
#include <string_view>
#include <utility>

namespace raftkv {

namespace {
constexpr char kDefaultCF[] = "default";
constexpr char KLockCF[] = "lock";
constexpr char kWriteCF[] = "write";

std::string_view ToSV(const rocksdb::Slice &s) {
  return std::string_view(s.data(), s.size());
}

MvccError StorageError(const std::string &key, const Status &s) {
  MvccError err;
  err.kind = MvccError::kStorageError;
  err.key = key;
  err.message = "storage write failed: " + s.error_msg;
  return err;
}

// user_key 是 key 的前缀扩展（如查 "acct_1" 撞到 "acct_10" 的版本）。
// 编码无分隔符，扩展键的版本块会插在 key 自己的版本区间之前/之中，
// 迭代时不能 break，要整块跳过。
bool IsPrefixExtension(std::string_view user_key, const std::string &key) {
  return user_key.size() > key.size() &&
         user_key.substr(0, key.size()) == key;
}

} // namespace

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
std::optional<std::pair<uint64_t, WriteInfo>>
MvccTxn::SeekWrite(const std::string &key, uint64_t ts) const {
  auto it = storage_->NewIterator(kWriteCF);
  // Seek 到 (key, ts)：因 ~ts 编码，落点即 commit_ts <= ts 的最新版本。
  for (it->Seek(MvccCodec::EncodeKey(key, ts)); it->Valid();) {
    std::string_view user_key;
    uint64_t commit_ts = 0;
    if (!MvccCodec::DecodeKey(ToSV(it->key()), &user_key, &commit_ts)) {
      break;
    }
    if (user_key != key) {
      if (IsPrefixExtension(user_key, key)) {
        it->Seek(MvccCodec::VersionRangeEnd(user_key));
        continue;
      }
      break; // 已越过该 key 的全部版本
    }
    auto info = WriteInfo::Deserialize(ToSV(it->value()));
    if (!info) {
      break;
    }
    if (info->kind == WriteInfo::kRollback) {
      it->Next();
      continue;
    }
    return std::make_pair(commit_ts, *info);
  }
  return std::nullopt;
}

std::optional<std::pair<uint64_t, WriteInfo>>
MvccTxn::GetTxnRecord(const std::string &key, uint64_t start_ts) const {
  // 从最新版本向旧版本迭代，找 start_ts 匹配的记录（含 rollback）。
  // commit_ts >= start_ts 恒成立，因此扫描到 commit_ts < start_ts 即可停。
  auto it = storage_->NewIterator(kWriteCF);
  for (it->Seek(MvccCodec::EncodeKey(key, UINT64_MAX)); it->Valid();) {
    std::string_view user_key;
    uint64_t commit_ts = 0;
    if (!MvccCodec::DecodeKey(ToSV(it->key()), &user_key, &commit_ts)) {
      break;
    }
    if (user_key != key) {
      if (IsPrefixExtension(user_key, key)) {
        it->Seek(MvccCodec::VersionRangeEnd(user_key));
        continue;
      }
      break;
    }
    if (commit_ts < start_ts) {
      break;
    }
    auto info = WriteInfo::Deserialize(ToSV(it->value()));
    if (info && info->start_ts == start_ts) {
      return std::make_pair(commit_ts, *info);
    }
    it->Next();
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// 读路径
// ---------------------------------------------------------------------------
MvccError MvccTxn::Get(const std::string &key, uint64_t snapshot_ts,
                       std::optional<std::string> *value) const {
  value->reset();
  // 1. 锁检查：存在 start_ts <= snapshot_ts 的锁 → 事务命运未定，不能读。
  //    （更晚的锁不影响本快照：即便它提交，commit_ts 也必然 > snapshot_ts。）
  std::string lock_data;
  if (storage_->Get(KLockCF, key, &lock_data)) {
    auto lock = LockInfo::Deserialize(lock_data);
    if (lock && lock->start_ts <= snapshot_ts) {
      MvccError err;
      err.kind = MvccError::kLocked;
      err.key = key;
      err.primary = lock->primary;
      err.lock_ts = lock->start_ts;
      err.message = "key is locked by an uncommitted transaction";
      return err;
    }
  }

  // 2. write CF 找快照内最新提交，按其 start_ts 回 default CF 取数据。
  auto write = SeekWrite(key, snapshot_ts);
  if (!write) {
    return MvccError::Ok(); // 快照内无可见版本
  }
  if (write->second.kind == WriteInfo::kDelete) {
    return MvccError::Ok();
  }
  std::string data;
  bool found = storage_->Get(
      kDefaultCF, MvccCodec::EncodeKey(key, write->second.start_ts), &data);
  assert(found); // write 记录存在则数据必然存在（同一 WriteBatch 写入）
  (void)found;
  *value = std::move(data);
  return MvccError::Ok();
}

MvccError
MvccTxn::Scan(const std::string &start_key, const std::string &end_key,
              int limit, uint64_t snapshot_ts,
              std::vector<std::pair<std::string, std::string>> *out) const {
  out->clear();
  // 以 write CF 为主索引迭代：每遇到一个新 user_key，
  // 用 SeekWrite/Get 语义取其快照内最新版本，然后跳到下一个 user_key。
  auto it = storage_->NewIterator(kWriteCF);
  std::string cursor = start_key;
  while (limit <= 0 || static_cast<int>(out->size()) < limit) {
    it->Seek(cursor);
    if (!it->Valid()) {
      break;
    }
    std::string_view user_key_sv;
    uint64_t commit_ts;
    if (!MvccCodec::DecodeKey(ToSV(it->key()), &user_key_sv, &commit_ts)) {
      break;
    }
    std::string user_key(user_key_sv);
    if (!end_key.empty() && user_key >= end_key) {
      break;
    }

    std::optional<std::string> value;
    MvccError err = Get(user_key, snapshot_ts, &value);
    if (!err.ok()) {
      return err;
    }
    if (value) {
      out->emplace_back(user_key, std::move(*value));
    }
    cursor = MvccCodec::VersionRangeEnd(user_key);
  }
  return MvccError::Ok();
}

// ---------------------------------------------------------------------------
// 写路径（必须在 Raft on_apply 内、单线程串行执行）
// ---------------------------------------------------------------------------
MvccError MvccTxn::Prewrite(const std::string &key, const std::string &value,
                            bool is_delete, const std::string &primary,
                            uint64_t start_ts, uint64_t ttl_ms) {
  // 1. 写冲突检查：write CF 里存在 commit_ts >= start_ts 的提交 →
  //    本事务开始后有人提交了该 key，SI 下必须失败重试。
  auto latest = SeekWrite(key, UINT64_MAX);
  if (latest && latest->first >= start_ts) {
    MvccError err;
    err.kind = MvccError::kWriteConflict;
    err.key = key;
    err.conflict_commit_ts = latest->first;
    err.message = "write conflict: newer committed version exists";
    return err;
  }

  // 2. rollback 墓碑检查：本事务（同 start_ts）已被回滚 → 拒绝迟到的
  //    Prewrite，防止「回滚后又复活」（unistore LockNotExist 防御）。
  auto record = GetTxnRecord(key, start_ts);
  if (record && record->second.kind == WriteInfo::kRollback) {
    MvccError err;
    err.kind = MvccError::kAlreadyRolledBack;
    err.key = key;
    err.message = "transaction was rolled back";
    return err;
  }

  // 3. 锁检查。
  std::string lock_data;
  if (storage_->Get(KLockCF, key, &lock_data)) {
    auto lock = LockInfo::Deserialize(lock_data);
    if (lock && lock->start_ts == start_ts) {
      return MvccError::Ok(); // 自己的锁（RPC 重试）：幂等成功
    }
    MvccError err;
    err.kind = MvccError::kLocked;
    err.key = key;
    if (lock) {
      err.primary = lock->primary;
      err.lock_ts = lock->start_ts;
    }
    err.message = "key is locked by another transaction";
    return err;
  }
  // 4. 上锁 + 写数据，同一个 WriteBatch 原子落盘。
  LockInfo lock;
  lock.start_ts = start_ts;
  lock.ttl_ms = ttl_ms;
  lock.lock_type = is_delete ? LockInfo::kDelete : LockInfo::kPut;
  lock.primary = primary;

  WriteBatch batch;
  batch.Put(KLockCF, key, lock.Serialize());
  if (!is_delete) {
    batch.Put(kDefaultCF, MvccCodec::EncodeKey(key, start_ts), value);
  }
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();
}

MvccError MvccTxn::Commit(const std::string &key, uint64_t start_ts,
                          uint64_t commit_ts) {
  std::string lock_data;
  if (!storage_->Get(KLockCF, key, &lock_data)) {
    // 锁不存在的三种可能：
    auto record = GetTxnRecord(key, start_ts);
    if (record && record->second.kind != WriteInfo::kRollback) {
      return MvccError::Ok(); // (a) 已提交（Commit RPC 重试）：幂等成功
    }
    // (b) 已回滚 / (c) 从未 prewrite：事务失败
    MvccError err;
    err.kind = MvccError::kTxnNotFound;
    err.key = key;
    err.message = "lock not found, txn may have been rolled back";
    return err;
  }
  auto lock = LockInfo::Deserialize(lock_data);
  if (!lock || lock->start_ts != start_ts) {
    MvccError err;
    err.kind = MvccError::kTxnNotFound;
    err.key = key;
    err.message = "lock is held by another transaction";
    return err;
  }
  WriteInfo write;
  write.start_ts = start_ts;
  write.kind = (lock->lock_type == LockInfo::kDelete) ? WriteInfo::kDelete
                                                      : WriteInfo::kPut;

  WriteBatch batch;
  batch.Put(kWriteCF, MvccCodec::EncodeKey(key, commit_ts), write.Serialize());
  batch.Delete(KLockCF, key);
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();
}

MvccError MvccTxn::Rollback(const std::string &key, uint64_t start_ts) {
  auto record = GetTxnRecord(key, start_ts);
  if (record) {
    if (record->second.kind == WriteInfo::kRollback) {
      return MvccError::Ok(); // 已回滚：幂等成功
    }
    MvccError err; // 已提交的事务不可回滚
    err.kind = MvccError::kTxnNotFound;
    err.key = key;
    err.message = "cannot rollback a committed transaction";
    return err;
  }
  WriteBatch batch;
  std::string lock_data;
  if (storage_->Get(KLockCF, key, &lock_data)) {
    auto lock = LockInfo::Deserialize(lock_data);
    if (lock && lock->start_ts == start_ts) {
      // 清锁 + 清数据。
      batch.Delete(KLockCF, key);
      batch.Delete(kDefaultCF, MvccCodec::EncodeKey(key, start_ts));
    }
  }
  // 无论锁在不在都写 rollback 墓碑（commit_ts == start_ts 的特殊记录），
  // 挡住可能迟到的 Prewrite。
  WriteInfo tomb;
  tomb.start_ts = start_ts;
  tomb.kind = WriteInfo::kRollback;
  batch.Put(kWriteCF, MvccCodec::EncodeKey(key, start_ts), tomb.Serialize());
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();
}
// ---------------------------------------------------------------------------
// 残锁处理（对标 unistore CheckTxnStatus 的四分支）
// ---------------------------------------------------------------------------
MvccError MvccTxn::CheckTxnStatus(const std::string &primary, uint64_t lock_ts,
                                  uint64_t now_ts, TxnStatus *status) {
  // 分支 1/2：primary 已有最终记录。
  auto record = GetTxnRecord(primary, lock_ts);
  if (record) {
    if (record->second.kind == WriteInfo::kRollback) {
      status->state = TxnStatus::kRolledBack;
    } else {
      status->state = TxnStatus::kCommitted;
      status->commit_ts = record->first;
    }
    return MvccError::Ok();
  }

  std::string lock_data;
  if (storage_->Get(KLockCF, primary, &lock_data)) {
    auto lock = LockInfo::Deserialize(lock_data);
    if (lock && lock->start_ts == lock_ts) {
      // 分支 3/4：锁还在，看 TTL。ts 高 18 位是物理毫秒
      uint64_t elapsed_ms = (now_ts >> 18) - (lock_ts >> 18);
      if (elapsed_ms < lock->ttl_ms) {
        status->state = TxnStatus::kLockAlive;
        status->remaining_ttl_ms = lock->ttl_ms - elapsed_ms;
        return MvccError::Ok();
      }
      // TTL 过期：当场回滚 primary —— 这一步决定了整个事务的命运。
      MvccError err = Rollback(primary, lock_ts);
      if (!err.ok()) {
        return err;
      }
      status->state = TxnStatus::kRolledBack;
      return MvccError::Ok();
    }
  }

  // 锁不在且无任何记录：可能 Prewrite 还没到达。写 rollback 墓碑
  // 把事务在该 key 上判死，防止之后迟到的 Prewrite 复活（关键防御！
  MvccError err = Rollback(primary, lock_ts);
  if (!err.ok()) {
    return err;
  }
  status->state = TxnStatus::kRolledBack;
  return MvccError::Ok();
}

MvccError MvccTxn::ResolveLock(const std::string &key, uint64_t start_ts,
                               uint64_t commit_ts) {
  if (commit_ts > 0) {
    return Commit(key, start_ts, commit_ts);
  }
  return Rollback(key, start_ts);
}
} // namespace raftkv