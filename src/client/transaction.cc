#include "client/transaction.h"

#include <butil/logging.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace raftkv {

// ── 事务读 ────────────────────────────────────────────────────────────

std::optional<std::string> Transaction::Get(const std::string &key) {
  // 读己之写：本地写缓冲优先于快照
  auto it = write_buffer_.find(key);
  if (it != write_buffer_.end()) {
    if (it->second.deleted) {
      return std::nullopt;
    }
    return it->second.value;
  }
  return SnapshotGetWithResolve(key);
}

std::vector<std::pair<std::string, std::string>>
Transaction::Scan(const std::string &start_key, const std::string &end_key,
                  int limit) {
  // 快照扫描（撞锁时 resolve 后重试），再叠加本地写缓冲
  std::vector<std::pair<std::string, std::string>> snap;
  for (int attempt = 0; attempt <= kMaxLockRetry; ++attempt) {
    LockInfo lock;
    std::string err;
    if (client_->SnapshotScan(start_key, end_key, limit, start_ts_, &snap,
                              &lock, &err)) {
      break;
    }
    if (lock.lock_ts != 0 && ResolveLock(lock, attempt)) {
      continue;
    }
    LOG(WARNING) << "Txn Scan 失败: " << err;
    return {};
  }

  // 合并：快照结果打底，缓冲区内 [start_key, end_key) 的写覆盖之
  std::map<std::string, std::string> merged(snap.begin(), snap.end());
  auto lo = write_buffer_.lower_bound(start_key);
  auto hi = end_key.empty() ? write_buffer_.end()
                            : write_buffer_.lower_bound(end_key);
  for (auto it = lo; it != hi; ++it) {
    if (it->second.deleted) {
      merged.erase(it->first);
    } else {
      merged[it->first] = it->second.value;
    }
  }

  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &kv : merged) {
    if (limit > 0 && static_cast<int>(result.size()) >= limit) {
      break;
    }
    result.emplace_back(kv.first, kv.second);
  }
  return result;
}

std::optional<std::string>
Transaction::SnapshotGetWithResolve(const std::string &key) {
  for (int attempt = 0; attempt <= kMaxLockRetry; ++attempt) {
    std::string value;
    bool found = false;
    LockInfo lock;
    std::string err;
    if (client_->SnapshotGet(key, start_ts_, &value, &found, &lock, &err)) {
      if (found) {
        return value;
      }
      return std::nullopt;
    }
    if (lock.lock_ts != 0 && ResolveLock(lock, attempt)) {
      continue;
    }
    LOG(WARNING) << "Txn Get 失败: " << err;
    return std::nullopt;
  }
  LOG(WARNING) << "Txn Get 撞锁重试耗尽: " << key;
  return std::nullopt;
}

bool Transaction::ResolveLock(const LockInfo &lock, int attempt) {
  // CheckTxnStatus 会推进对端事务状态：
  //   committed → 锁应按 commit_ts 落盘，重读即可见；
  //   remaining_ttl == 0 → 已回滚 / 被本次调用回滚，重读锁消失；
  //   remaining_ttl > 0 → 对端事务还活着，backoff 后重试。
  TxnStatus status;
  std::string err;
  if (!client_->CheckTxnStatus(lock.primary, lock.lock_ts, &status, &err)) {
    LOG(WARNING) << "CheckTxnStatus 失败: " << err;
    return false;
  }
  if (status.committed) {
    // primary 已提交：用同一 commit_ts 帮助推进被锁的 key（secondary），
    // 否则残锁会一直挡住读。失败无害（可能已被别的读者推进），重读即可。
    std::string resolve_err;
    client_->TxnCommit({lock.key}, lock.lock_ts, status.commit_ts, nullptr,
                       &resolve_err);
  } else if (status.remaining_ttl_ms == 0) {
    // 对端已回滚（或刚被本次 CheckTxnStatus 回滚）：清掉该 key 上的残锁
    std::string resolve_err;
    client_->TxnRollback({lock.key}, lock.lock_ts, &resolve_err);
  } else {
    // 对端事务还活着：指数退避，等它提交或 TTL 过期
    uint64_t sleep_ms = std::min<uint64_t>(50ULL << attempt, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return true;
}

// ── 两阶段提交 ────────────────────────────────────────────────────────

bool Transaction::Commit(std::string *err) {
  if (finished_) {
    if (err) *err = "事务已结束";
    return false;
  }
  if (start_ts_ == 0) {
    finished_ = true;
    if (err) *err = "start_ts 获取失败，事务不可用";
    return false;
  }
  // 只读事务：无需 2PC
  if (write_buffer_.empty()) {
    finished_ = true;
    return true;
  }

  // 阶段一：Prewrite 全部 mutation，primary 取最小 key（重试时稳定）
  std::vector<TxnMutation> mutations;
  mutations.reserve(write_buffer_.size());
  for (const auto &kv : write_buffer_) {
    TxnMutation m;
    m.key = kv.first;
    m.value = kv.second.value;
    m.is_delete = kv.second.deleted;
    mutations.push_back(std::move(m));
  }
  const std::string &primary = write_buffer_.begin()->first;

  LockInfo conflict;
  std::string prewrite_err;
  if (!client_->TxnPrewrite(mutations, primary, start_ts_, kLockTtlMs,
                            &conflict, &prewrite_err)) {
    // 乐观模型：冲突即整个事务失败，清掉自己可能留下的锁后由调用方重跑
    std::string rollback_err;
    client_->TxnRollback(BufferedKeys(), start_ts_, &rollback_err);
    finished_ = true;
    if (err) {
      *err = conflict.lock_ts != 0
                 ? "写冲突: key=" + conflict.key +
                       " 被事务 lock_ts=" + std::to_string(conflict.lock_ts) +
                       " 占用（" + prewrite_err + "）"
                 : prewrite_err;
    }
    return false;
  }

  // 阶段二：commit_ts 传 0，由 leader TSO 分配并随日志复制
  std::string commit_err;
  if (!client_->TxnCommit(BufferedKeys(), start_ts_, /*commit_ts=*/0,
                          &commit_ts_, &commit_err)) {
    // 提交是否生效未知，不能盲目回滚（可能已提交）；
    // 残锁留给后续读者经 CheckTxnStatus 判定。
    finished_ = true;
    if (err) *err = "Commit 失败: " + commit_err;
    return false;
  }

  finished_ = true;
  return true;
}

void Transaction::Rollback() {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (write_buffer_.empty() || start_ts_ == 0) {
    return; // 从未 Prewrite，服务端无锁可清
  }
  std::string err;
  if (!client_->TxnRollback(BufferedKeys(), start_ts_, &err)) {
    LOG(WARNING) << "Txn Rollback 失败（残锁将由 TTL 过期回收）: " << err;
  }
}

std::vector<std::string> Transaction::BufferedKeys() const {
  std::vector<std::string> keys;
  keys.reserve(write_buffer_.size());
  for (const auto &kv : write_buffer_) {
    keys.push_back(kv.first);
  }
  return keys;
}

} // namespace raftkv
