#include "raft/kv_state_machine.h"

#include <atomic>
#include <braft/raft.h>
#include <butil/iobuf.h>
#include <butil/logging.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "braft/storage.h"
#include "braft/util.h"
#include "kv.pb.h"
#include "service/kv_service.h"
#include "storage/mvcc_txn.h"
#include "storage/rocksdb_storage.h"

namespace raftkv {
namespace {
// meta CF 中持久化 TSO 预留上界的键
constexpr char kTsoReservedKey[] = "__tso_reserved__";
} // namespace

KVStateMachine::KVStateMachine(std::shared_ptr<RocksDbStorage> storage)
    : storage_(std::move(storage)), mvcc_(storage_.get()) {
  LoadTsoReserved();
}

void KVStateMachine::LoadTsoReserved() {
  std::string value;
  if (storage_->GetMeta(kTsoReservedKey, &value)) {
    const uint64_t reserved = strtoull(value.c_str(), nullptr, 10);
    uint64_t cur = tso_reserved_.load(std::memory_order_relaxed);
    if (reserved > cur) {
      tso_reserved_.store(reserved, std::memory_order_release);
    }
  }
}

namespace {
// 把 MvccError 写进事务响应的公共字段（success/error）
template <typename ResponseType>
void FillTxnError(ResponseType *resp, const MvccError &err) {
  resp->set_success(false);
  resp->set_error(err.message);
}
} // namespace

// ── on_apply：核心函数，应用提交的日志 ──────────────────────────────────
// void KVStateMachine::on_apply(braft::Iterator &iter) {
//   int64_t last_index = 0;
//   std::vector<std::pair<std::string, std::string>> puts;
//   std::vector<std::string> deletes;

//   for (; iter.valid(); iter.next()) {
//     braft::AsyncClosureGuard closure_guard(iter.done());

//     int64_t log_index = iter.index();
//     last_index = iter.index();
//     butil::IOBuf data = iter.data();

//     kv::KvOperation operation;
//     butil::IOBufAsZeroCopyInputStream wrapper(data);
//     if (!operation.ParseFromZeroCopyStream(&wrapper)) {
//       LOG(ERROR) << "解析 KvOperation 失败 [index=" << log_index << "]";
//       continue;
//     }
//     // RocksDB 内部有锁，on_apply 串行执行，无需外层 mutex
//     switch (operation.op()) {
//     case kv::OP_PUT: {
//       puts.emplace_back(operation.key(), operation.value());
//       break;
//     }
//     case kv::OP_DELETE: {
//       deletes.push_back(operation.key());
//       break;
//     }
//     case kv::OP_GET: {
//       // 线性一致读：日志仅用于建立 apply 顺序点，实际读在
//       // ReadIndexClosure::Run()
//       VLOG(1) << "GET [index=" << log_index << ", key=" << operation.key()
//               << "]";
//       break;
//     }
//     case kv::OP_SCAN: {
//       // 线性一致 Scan：同上，实际读在 ScanReadIndexClosure::Run()
//       VLOG(1) << "SCAN [index=" << log_index << "]";
//       break;
//     }

//     case kv::OP_TXN_PREWRITE: {
//       kv::TxnPrewriteRequest req;
//       req.ParseFromString(operation.value());
//       // 逐 mutation prewrite；日志序列在各副本一致，中途失败也确定性一致，
//       // 遗留的锁由客户端 rollback / resolve 清理。
//       MvccError err = MvccError::Ok();
//       for (const auto &m : req.mutations()) {
//         err = mvcc_.Prewrite(m.key(), m.value(), m.op() ==
//         kv::Mutation::DELETE,
//                              req.primary(), req.start_ts(), req.ttl_ms());
//         if (!err.ok()) {
//           break;
//         }
//       }
//       // iter.done() != nullptr 仅在 leader 上成立：把结果写回 response
//       auto *c =
//           dynamic_cast<TxnClosure<kv::TxnPrewriteResponse> *>(iter.done());
//       if (c != nullptr) {
//         auto *resp = c->response();
//         if (err.ok()) {
//           resp->set_success(true);
//         } else {
//           FillTxnError(resp, err);
//           resp->set_conflict_key(err.key);
//           resp->set_conflict_primary(err.primary);
//           resp->set_conflict_start_ts(err.lock_ts);
//           resp->set_conflict_commit_ts(err.conflict_commit_ts);
//         }
//       }
//       break;
//     }

//     case kv::OP_TXN_COMMIT: {
//       kv::TxnCommitRequest req;
//       req.ParseFromString(operation.value());
//       MvccError err = MvccError::Ok();
//       for (const auto &key : req.keys()) {
//         err = mvcc_.Commit(key, req.start_ts(), req.commit_ts());
//         if (!err.ok()) {
//           break;
//         }
//       }
//       auto *c = static_cast<TxnClosure<kv::TxnCommitResponse>
//       *>(iter.done()); if (c != nullptr) {
//         auto *resp = c->response();
//         if (err.ok()) {
//           resp->set_success(true);
//           resp->set_commit_ts(req.commit_ts()); // handler 已确保非 0
//         } else {
//           FillTxnError(resp, err);
//         }
//       }
//       break;
//     }

//     case kv::OP_TXN_ROLLBACK: {
//       kv::TxnRollbackRequest req;
//       req.ParseFromString(operation.value());
//       MvccError err = MvccError::Ok();
//       for (const auto &key : req.keys()) {
//         err = mvcc_.Rollback(key, req.start_ts());
//         if (!err.ok()) {
//           break;
//         }
//       }
//       auto *c =
//           dynamic_cast<TxnClosure<kv::TxnRollbackResponse> *>(iter.done());
//       if (c != nullptr) {
//         auto *resp = c->response();
//         if (err.ok()) {
//           resp->set_success(true);
//         } else {
//           FillTxnError(resp, err);
//         }
//       }
//       break;
//     }

//     case kv::OP_TXN_RESOLVE: {
//       kv::CheckTxnStatusRequest req;
//       req.ParseFromString(operation.value());
//       // now_ts 由 leader 在 handler 里定好并随日志复制（复用 key 字段），
//       // 保证 TTL 判定在所有副本上确定性一致。
//       uint64_t now_ts = strtoull(operation.key().c_str(), nullptr, 10);
//       TxnStatus status;
//       MvccError err =
//           mvcc_.CheckTxnStatus(req.primary(), req.lock_ts(), now_ts,
//           &status);
//       auto *c =
//           dynamic_cast<TxnClosure<kv::CheckTxnStatusResponse>
//           *>(iter.done());
//       if (c != nullptr) {
//         auto *resp = c->response();
//         if (err.ok()) {
//           resp->set_success(true);
//           resp->set_committed(status.state == TxnStatus::kCommitted);
//           resp->set_commit_ts(status.commit_ts);
//           resp->set_remaining_ttl_ms(status.remaining_ttl_ms);
//         } else {
//           FillTxnError(resp, err);
//         }
//       }
//       break;
//     }

//     default:
//       LOG(WARNING) << "未知操作 [index=" << log_index
//                    << ", op=" << operation.op() << "]";
//       break;
//     }
//   }
//   if (!puts.empty() || !deletes.empty()) {
//     auto s = storage_->BatchWrite(puts, deletes);
//     if (!s) {
//       LOG(ERROR) << "BatchWrite 失败: " << s.error_msg;
//     }
//   }
//   if (last_index > 0) {
//     last_applied_index_.store(last_index, std::memory_order_release);
//     applied_cv_.notify_all();
//   }
// }
void KVStateMachine::on_apply(braft::Iterator &iter) {
  int64_t last_index = 0;
  WriteBatch batch; // 按日志顺序累积，保序
  std::vector<braft::Closure *>
      pending_closures; // OP_PUT/OP_DELETE 的延迟 closure

  for (; iter.valid(); iter.next()) {
    braft::AsyncClosureGuard closure_guard(iter.done());

    int64_t log_index = iter.index();
    last_index = iter.index();
    butil::IOBuf data = iter.data();

    kv::KvOperation operation;
    butil::IOBufAsZeroCopyInputStream wrapper(data);
    if (!operation.ParseFromZeroCopyStream(&wrapper)) {
      LOG(ERROR) << "解析 KvOperation 失败 [index=" << log_index << "]";
      continue;
    }
    switch (operation.op()) {
    case kv::OP_PUT: {
      batch.Put("data", operation.key(), operation.value());
      if (braft::Closure *done = iter.done()) {
        closure_guard
            .release(); // 注意：返回 google::protobuf::Closure*，丢弃即可
        pending_closures.push_back(
            done); // 用 iter.done() 的类型化指针，落盘后再应答
      }
      break;
    }
    case kv::OP_DELETE: {
      batch.Delete("data", operation.key());
      if (braft::Closure *done = iter.done()) {
        closure_guard.release();
        pending_closures.push_back(done);
      }
      break;
    }
    case kv::OP_GET: {
      // 线性一致读：日志仅用于建立 apply 顺序点，实际读在
      // ReadIndexClosure::Run()
      VLOG(1) << "GET [index=" << log_index << ", key=" << operation.key()
              << "]";
      break;
    }
    case kv::OP_SCAN: {
      // 线性一致 Scan：同上，实际读在 ScanReadIndexClosure::Run()
      VLOG(1) << "SCAN [index=" << log_index << "]";
      break;
    }

    case kv::OP_TXN_PREWRITE: {
      kv::TxnPrewriteRequest req;
      req.ParseFromString(operation.value());
      // 逐 mutation prewrite；日志序列在各副本一致，中途失败也确定性一致，
      // 遗留的锁由客户端 rollback / resolve 清理。
      MvccError err = MvccError::Ok();
      for (const auto &m : req.mutations()) {
        err = mvcc_.Prewrite(m.key(), m.value(), m.op() == kv::Mutation::DELETE,
                             req.primary(), req.start_ts(), req.ttl_ms());
        if (!err.ok()) {
          break;
        }
      }
      // iter.done() != nullptr 仅在 leader 上成立：把结果写回 response
      auto *c =
          dynamic_cast<TxnClosure<kv::TxnPrewriteResponse> *>(iter.done());
      if (c != nullptr) {
        auto *resp = c->response();
        if (err.ok()) {
          resp->set_success(true);
        } else {
          FillTxnError(resp, err);
          resp->set_conflict_key(err.key);
          resp->set_conflict_primary(err.primary);
          resp->set_conflict_start_ts(err.lock_ts);
          resp->set_conflict_commit_ts(err.conflict_commit_ts);
        }
      }
      break;
    }

    case kv::OP_TXN_COMMIT: {
      kv::TxnCommitRequest req;
      req.ParseFromString(operation.value());
      // 让 follower 的 TSO 跟随日志中的 commit_ts，failover 后不回退
      tso_.RecoverTo(req.commit_ts());
      MvccError err = MvccError::Ok();
      // keys 由客户端按序传入，keys[0] 即 primary（write_buffer_ 是 std::map）。
      // primary 先提交决定事务命运：它失败则整个事务未提交，后续 key 不再动。
      int committed = 0;
      for (const auto &key : req.keys()) {
        err = mvcc_.Commit(key, req.start_ts(), req.commit_ts());
        if (!err.ok()) {
          if (committed > 0) {
            // primary 已提交却有 secondary 提交不掉：原子性告警（不应发生，
            // resolve 链路只会依 primary 状态推进 secondary）。
            LOG(ERROR) << "OP_TXN_COMMIT 部分提交! start_ts=" << req.start_ts()
                       << " commit_ts=" << req.commit_ts() << " key=" << key
                       << " err=" << err.message;
          }
          break;
        }
        ++committed;
      }
      auto *c = static_cast<TxnClosure<kv::TxnCommitResponse> *>(iter.done());
      if (c != nullptr) {
        auto *resp = c->response();
        if (err.ok()) {
          resp->set_success(true);
          resp->set_commit_ts(req.commit_ts()); // handler 已确保非 0
        } else {
          FillTxnError(resp, err);
        }
      }
      break;
    }

    case kv::OP_TXN_ROLLBACK: {
      kv::TxnRollbackRequest req;
      req.ParseFromString(operation.value());
      MvccError err = MvccError::Ok();
      for (const auto &key : req.keys()) {
        err = mvcc_.Rollback(key, req.start_ts());
        if (!err.ok()) {
          break;
        }
      }
      auto *c =
          dynamic_cast<TxnClosure<kv::TxnRollbackResponse> *>(iter.done());
      if (c != nullptr) {
        auto *resp = c->response();
        if (err.ok()) {
          resp->set_success(true);
        } else {
          FillTxnError(resp, err);
        }
      }
      break;
    }

    case kv::OP_TXN_RESOLVE: {
      kv::CheckTxnStatusRequest req;
      req.ParseFromString(operation.value());
      // now_ts 由 leader 在 handler 里定好并随日志复制（复用 key 字段），
      // 保证 TTL 判定在所有副本上确定性一致。
      uint64_t now_ts = strtoull(operation.key().c_str(), nullptr, 10);
      TxnStatus status;
      MvccError err =
          mvcc_.CheckTxnStatus(req.primary(), req.lock_ts(), now_ts, &status);
      auto *c =
          dynamic_cast<TxnClosure<kv::CheckTxnStatusResponse> *>(iter.done());
      if (c != nullptr) {
        auto *resp = c->response();
        if (err.ok()) {
          resp->set_success(true);
          resp->set_committed(status.state == TxnStatus::kCommitted);
          resp->set_commit_ts(status.commit_ts);
          resp->set_remaining_ttl_ms(status.remaining_ttl_ms);
        } else {
          FillTxnError(resp, err);
        }
      }
      break;
    }

    case kv::OP_TSO_RESERVE: {
      // 预留式发号：日志携带 leader 选定的「未来上界」，
      // apply 后 leader 才允许发号到该上界；meta CF 随快照持久化。
      const uint64_t reserve = strtoull(operation.key().c_str(), nullptr, 10);
      uint64_t cur = tso_reserved_.load(std::memory_order_relaxed);
      if (reserve > cur) {
        tso_reserved_.store(reserve, std::memory_order_release);
        batch.Put("meta", kTsoReservedKey, operation.key());
      }
      break;
    }

    default:
      LOG(WARNING) << "未知操作 [index=" << log_index
                   << ", op=" << operation.op() << "]";
      break;
    }
  }
  // 单个 WriteBatch 原子落盘（保持日志原顺序），成功后统一应答。
  bool write_ok = true;
  if (!batch.empty()) {
    auto s = storage_->Write(std::move(batch));
    if (!s) {
      write_ok = false;
      LOG(ERROR) << "on_apply 批量落盘失败: " << s.error_msg;
    }
  }
  for (braft::Closure *done : pending_closures) {
    if (!write_ok) {
      done->status().set_error(EIO, "storage write failed");
    }
    // 与 AsyncClosureGuard 的析构语义保持一致：closure 在 bthread 里运行，
    // 避免阻塞 on_apply 所在的 FSM 线程。
    braft::run_closure_in_bthread(done);
  }

  // 只有真正落盘之后才推进 applied index（Log Read barrier 依赖它）。
  if (write_ok && last_index > 0) {
    last_applied_index_.store(last_index, std::memory_order_release);
    applied_cv_.notify_all();
  }
}

// ── on_snapshot_save：RocksDB Checkpoint（硬链接，毫秒级）─────────────
void KVStateMachine::on_snapshot_save(braft::SnapshotWriter *writer,
                                      braft::Closure *done) {
  std::string checkpoint_path = writer->get_path() + "/checkpoint";
  auto s = storage_->CreateCheckpoint(checkpoint_path);
  if (!s) {
    LOG(ERROR) << "CreateCheckpoint 失败: " << s.error_msg;
    done->status().set_error(EIO, "CreateCheckpoint failed");
    done->Run();
    return;
  }
  // 告诉 braft 快照包含 checkpoint/ 整个目录
  if (writer->add_file("checkpoint/") != 0) {
    LOG(ERROR) << "add_file checkpoint/ 失败";
    done->status().set_error(EIO, "add_file failed");
    done->Run();
    return;
  }
  LOG(INFO) << "Snapshot saved via Checkpoint: " << checkpoint_path;
  done->Run();
}

// ── on_snapshot_load：从 Checkpoint 目录恢复 ─────────────────────────
int KVStateMachine::on_snapshot_load(braft::SnapshotReader *reader) {
  if (reader->get_file_meta("checkpoint/", nullptr) != 0) {
    LOG(ERROR) << "Snapshot 中不包含 checkpoint/ 目录";
    return -1;
  }
  std::string checkpoint_path = reader->get_path() + "/checkpoint";
  auto s = storage_->RestoreFromCheckpoint(checkpoint_path);
  if (!s) {
    LOG(ERROR) << "RestoreFromCheckpoint 失败: " << s.error_msg;
    return -1;
  }
  LOG(INFO) << "Snapshot loaded from Checkpoint: " << checkpoint_path;
  LoadTsoReserved();
  return 0;
}
// ── Leader 变更通知 ───────────────────────────────────────────────────
void KVStateMachine::on_leader_start(int64_t term) {
  LOG(INFO) << "=== Became LEADER at term=" << term << " ===";
  // 旧 leader 最多发号到已持久化的预留上界，直接跳过整段，
  // 绝不重复发号（⚠️ 不能从 now() 重启：新 leader 墙钟可能偏慢）。
  tso_.RecoverTo(tso_reserved_.load(std::memory_order_acquire));
  leader_term_.store(term, std::memory_order_release);
  is_leader_.store(true, std::memory_order_release);
}
void KVStateMachine::on_leader_stop(const butil::Status &status) {
  LOG(INFO) << "=== Stopped being LEADER: " << status << " ===";
  leader_term_.store(-1, std::memory_order_release);
  is_leader_.store(false, std::memory_order_release);
}

// ── 业务读接口（不走 Raft）────────────────────────────────────────────
bool KVStateMachine::Get(const std::string &key, std::string *value) const {
  return storage_->Get(key, value).ok;
}

MvccError KVStateMachine::TxnGet(const std::string &key, uint64_t snapshot_ts,
                                 std::optional<std::string> *value) const {
  return mvcc_.Get(key, snapshot_ts, value);
}

std::vector<std::pair<std::string, std::string>>
KVStateMachine::Scan(const std::string &start_key, const std::string &end_key,
                     int limit) const {
  return storage_->Scan(start_key, end_key, limit);
}

MvccError KVStateMachine::TxnScan(
    const std::string &start_key, const std::string &end_key, int limit,
    uint64_t snapshot_ts,
    std::vector<std::pair<std::string, std::string>> *out) const {
  return mvcc_.Scan(start_key, end_key, limit, snapshot_ts, out);
}

bool KVStateMachine::WaitApplied(int64_t target, int64_t timeout_ms) const {
  if (last_applied_index_.load(std::memory_order_acquire) >= target) {
    return true;
  }
  std::unique_lock<std::mutex> lock(applied_mutex_);
  return applied_cv_.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), [this, target] {
        return last_applied_index_.load(std::memory_order_acquire) >= target;
      });
}
} // namespace raftkv