#include "service/kv_service.h"
#include "braft/configuration.h"
#include "braft/raft.h"
#include "kv.pb.h"

#include <braft/util.h>
#include <butil/iobuf.h>
#include <butil/logging.h>
#include <butil/time.h>
#include <algorithm>
#include <optional>
#include <sched.h>
#include <vector>

namespace raftkv {
namespace {
// 混合时间戳：高 46 位物理毫秒 + 低 18 位逻辑位（与 TSO 布局一致）
uint64_t HybridNowTs() {
  return static_cast<uint64_t>(butil::gettimeofday_ms()) << 18;
}

// GetTso 单次批量上限：过大的 count 会让逻辑位借到物理位，ts 冲到墙钟前面
constexpr uint32_t kMaxTsoBatch = 1u << 18;

template <typename ResponseType>
void FillMvccReadError(ResponseType *response, const MvccError &err) {
  response->set_success(false);
  response->set_error(err.message);

  if (err.kind == MvccError::kLocked) {
    auto *lock = response->mutable_lock_conflict();
    lock->set_key(err.key);
    lock->set_primary(err.primary);
    lock->set_lock_ts(err.lock_ts);
  }
}
} // namespace

void FillGetResponse(const kv::GetRequest &request, kv::GetResponse *response,
                     KVStateMachine *fsm) {
  response->clear_error();
  response->clear_value();
  response->clear_lock_conflict();
  response->set_found(false);

  if (request.snapshot_ts() == 0) {
    std::string value;
    bool found = fsm->Get(request.key(), &value);
    response->set_success(true);
    response->set_found(found);

    if (found) {
      response->set_value(value);
    }
    return;
  }

  std::optional<std::string> value;
  MvccError err = fsm->TxnGet(request.key(), request.snapshot_ts(), &value);
  if (!err.ok()) {
    FillMvccReadError(response, err);
    return;
  }

  response->set_success(true);
  response->set_found(value.has_value());
  if (value) {
    response->set_value(*value);
  }
}
void FillScanResponse(const kv::ScanRequest &request,
                      kv::ScanResponse *response, KVStateMachine *fsm) {
  response->clear_error();
  response->clear_kvs();
  response->clear_lock_conflict();

  if (request.snapshot_ts() == 0) {
    auto kvs =
        fsm->Scan(request.start_key(), request.end_key(), request.limit());
    response->set_success(true);
    for (const auto &[key, value] : kvs) {
      auto *pair = response->add_kvs();
      pair->set_key(key);
      pair->set_value(value);
    }
    return;
  }
  std::vector<std::pair<std::string, std::string>> kvs;
  MvccError err = fsm->TxnScan(request.start_key(), request.end_key(),
                               request.limit(), request.snapshot_ts(), &kvs);
  if (!err.ok()) {
    FillMvccReadError(response, err);
    return;
  }
  response->set_success(true);
  for (const auto &[key, value] : kvs) {
    auto *pair = response->add_kvs();
    pair->set_key(key);
    pair->set_value(value);
  }
}

// ── 内部工具：非 Leader 时填充 redirect ─────────────────────────────
template <typename ResponseType>
bool KVServiceImpl::RediretIfNotLeader(ResponseType *response,
                                       google::protobuf::Closure *done) {
  if (node_->is_leader()) {
    return false;
  }
  braft::PeerId leader = node_->leader_id();
  response->set_success(false);
  if (leader.is_empty()) {
    response->set_error("no leader");
  } else {
    response->set_redirect(leader.to_string());
  }
  done->Run();
  return true;
}

// ── 内部工具：把事务请求打包成 KvOperation 日志并提交 raft ──────────
// req_payload：已序列化的事务请求；closure 所有权移交给 braft
template <typename ResponseType>
void KVServiceImpl::ApplyTxnOp(kv::OperationType op_type,
                               std::string req_payload, ResponseType *response,
                               google::protobuf::Closure *done,
                               std::string log_key) {
  kv::KvOperation op;
  op.set_op(op_type);
  op.set_key(std::move(log_key));
  op.set_value(std::move(req_payload));

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }
  braft::Task task;
  task.data = &log;
  task.done = new TxnClosure<ResponseType>(response, done);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}

// ── 内部工具：在已持久化的 TSO 预留上界内分配 [start, start+count) ──
// 预留式发号（先持久化、后发号）：发号与上界检查在 NextBatchBounded
// 的同一 CAS 循环内完成，任何已发出的号都 <= 已持久化上界；
// 上界不足时提交 OP_TSO_RESERVE 日志，apply 成功后重试。
// failover 时新 leader RecoverTo(上界)，绝不重复发号。
void KVServiceImpl::AllocateTso(
    uint32_t count, int attempt,
    std::function<void(const butil::Status &, uint64_t)> cont) {
  uint64_t start = 0;
  if (fsm_->Tso().NextBatchBounded(count, fsm_->TsoReserved(), &start)) {
    cont(butil::Status::OK(), start);
    return;
  }
  // 并发预留竞争/时钟快进时最多重试几次，避免无限递归
  constexpr int kMaxReserveAttempts = 8;
  if (attempt >= kMaxReserveAttempts) {
    cont(butil::Status(EAGAIN, "TSO 预留重试次数耗尽"), 0);
    return;
  }
  // 新上界覆盖当前水位与墙钟 + 本次需求 + 一整段预留，
  // 避免每次发号都写日志
  const uint64_t new_reserve =
      std::max(fsm_->Tso().Current(), HybridNowTs()) + count +
      KVStateMachine::kTsoReserveAhead;

  kv::KvOperation op;
  op.set_op(kv::OP_TSO_RESERVE);
  op.set_key(std::to_string(new_reserve));

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    cont(butil::Status(EINVAL, "序列化失败"), 0);
    return;
  }
  braft::Task task;
  task.data = &log;
  task.done = new TsoReserveClosure(
      [this, count, attempt, cont = std::move(cont)](const butil::Status &s) {
        if (!s.ok()) {
          cont(s, 0);
          return;
        }
        AllocateTso(count, attempt + 1, cont);
      });
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}

// ── GetTso RPC：leader 本地发号；预留上界不足时先走一条 Raft 日志 ────
void KVServiceImpl::GetTso(::google::protobuf::RpcController * /*controller*/,
                           const kv::TsoRequest *request,
                           kv::TsoResponse *response,
                           ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  uint32_t count = request->count() == 0 ? 1 : request->count();
  if (count > kMaxTsoBatch) {
    count = kMaxTsoBatch;
  }
  AllocateTso(count, 0,
              [response, done](const butil::Status &s, uint64_t start) {
                if (!s.ok()) {
                  response->set_success(false);
                  response->set_error(s.error_str());
                  done->Run();
                  return;
                }
                response->set_success(true);
                response->set_ts(start); // 区间 [ts, ts+count)
                done->Run();
              });
}

// ── Get RPC：支持弱一致读 / 线性一致读（走 Raft apply）───────────────
// ── Get RPC：snapshot_ts > 0 自动按线性一致读处理 ────────────────────

#if 0
void KVServiceImpl::Get(::google::protobuf::RpcController * /*controller*/,
                        const kv::GetRequest *request,
                        kv::GetResponse *response,
                        ::google::protobuf::Closure *done) {
  if (request->linearizable()) {
    // ── 线性一致读：将 OP_GET 提交 Raft 日志，在 on_apply 后读状态机 ──
    if (RediretIfNotLeader(response, done)) {
      return;
    }
    // ── Lease Read：租约有效时直接读状态机 ──
    if (node_->is_leader_lease_valid()) {
      // 获取当前 committed_index，等待 apply 追上
      braft::NodeStatus status;
      node_->get_status(&status);
      int64_t target = status.committed_index;

      if (!fsm_->WaitApplied(target, /*timeout_ms=*/50)) {
        LOG(WARNING) << "Lease Read: applied index 未追上 committed=" << target
                     << " applied=" << fsm_->LastAppliedIndex() << "，降级读";
      }
      // 直接读状态机，不写 Raft 日志
      std::string value;
      bool found = fsm_->Get(request->key(), &value);
      response->set_success(true);
      response->set_found(found);
      if (found) {
        response->set_value(value);
      }
      done->Run();
      return;
    }
    // ── 降级：租约无效，回退到 Log Read ──
    // 租约无效或 applied barrier 超时：追加一个只建立顺序点的 OP_GET 日志。
    // 实际读取由 ReadIndexClosure::Run 调用 FillGetResponse 完成。
    kv::KvOperation op;
    op.set_op(kv::OP_GET);
    op.set_key(request->key());

    butil::IOBuf log;
    butil::IOBufAsZeroCopyOutputStream stream(&log);
    if (!op.SerializeToZeroCopyStream(&stream)) {
      response->set_success(false);
      response->set_error("序列化失败");
      done->Run();
      return;
    }
    braft::Task task;
    task.data = &log;
    task.done = new ReadIndexClosure(request, response, done, fsm_);
    task.expected_term = fsm_->LeaderTerm();
    node_->apply(task);
    return;
  }
  // ── 弱一致读：直接读状态机 ──
  std::string value;
  bool found = fsm_->Get(request->key(), &value);
  response->set_success(true);
  response->set_found(found);
  if (found) {
    response->set_value(value);
  }
  done->Run();
}
#endif

void KVServiceImpl::Get(::google::protobuf::RpcController * /*controller*/,
                        const kv::GetRequest *request,
                        kv::GetResponse *response,
                        ::google::protobuf::Closure *done) {
  // 当前没有 follower safe-ts。即使调用方没有设置 linearizable，
  // MVCC 快照读也必须在 leader 上经过 applied barrier。
  const bool need_barrier =
      request->linearizable() || request->snapshot_ts() > 0;

  if (!need_barrier) {
    FillGetResponse(*request, response, fsm_);
    done->Run();
    return;
  }
  if (RediretIfNotLeader(response, done)) {
    return;
  }

  // fsm_->IsLeader() 与 node_->is_leader() 的关键区别：前者在 on_leader_start
  // 里置位，而 on_leader_start 按 apply 顺序投递，因此它为真时状态机必然已经
  // 应用完前任 leader 提交的全部日志。node_->is_leader() 在选举一结束就为真，
  // 此时 applied index 可能远远落后（极端情况：节点刚重启、RocksDB 被清空、
  // 正在从头重放日志，而 get_status().committed_index 也还没恢复到真实水位），
  // 用它做 lease read 会读到旧快照，基于该快照的事务随后能在 Prewrite 冲突
  // 检查中「合法」通过，从而覆盖更新的提交 —— 丢失更新。
  if (fsm_->IsLeader() && node_->is_leader_lease_valid()) {
    braft::NodeStatus status;
    node_->get_status(&status);
    const int64_t target = status.committed_index;

    if (fsm_->WaitApplied(target, 50)) {
      FillGetResponse(*request, response, fsm_);
      done->Run();
      return;
    }
    // 不能在 WaitApplied 超时后继续直接读，否则不再满足线性一致性。
    LOG(WARNING) << "Lease Read: applied index 未追上 committed=" << target
                 << " applied=" << fsm_->LastAppliedIndex()
                 << "，回退到 Log Read";
  }
  // 租约无效或 applied barrier 超时：追加一个只建立顺序点的 OP_GET 日志。
  // 实际读取由 ReadIndexClosure::Run 调用 FillGetResponse 完成。
  kv::KvOperation op;
  op.set_op(kv::OP_GET);
  op.set_key(request->key());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }

  braft::Task task;
  task.data = &log;
  task.done = new ReadIndexClosure(request, response, done, fsm_);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}

// ── Put RPC：写操作，需经过 Raft 共识 ────────────────────────────────
void KVServiceImpl::Put(::google::protobuf::RpcController * /*controller*/,
                        const kv::PutRequest *request,
                        kv::PutResponse *response,
                        ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  kv::KvOperation op;
  op.set_op(kv::OP_PUT);
  op.set_key(request->key());
  op.set_value(request->value());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }
  braft::Task task;
  task.data = &log;
  task.done = new KVClosure<kv::PutResponse>(response, done);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}

// ── Delete RPC：写操作，需经过 Raft 共识 ─────────────────────────────
void KVServiceImpl::Delete(::google::protobuf::RpcController * /*controller*/,
                           const kv::DeleteRequest *request,
                           kv::DeleteResponse *response,
                           ::google::protobuf::Closure *done) {

  if (RediretIfNotLeader(response, done)) {
    return;
  }

  kv::KvOperation op;
  op.set_op(kv::OP_DELETE);
  op.set_key(request->key());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }
  braft::Task task;
  task.data = &log;
  task.done = new KVClosure<kv::DeleteResponse>(response, done);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}

#if 0
// ── Scan RPC：支持弱一致读 / 线性一致读（走 Raft apply）───────────────
void KVServiceImpl::Scan(::google::protobuf::RpcController * /*controller*/,
                         const kv::ScanRequest *request,
                         kv::ScanResponse *response,
                         ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->linearizable()) {
    if (node_->is_leader_lease_valid()) {
      braft::NodeStatus status;
      node_->get_status(&status);
      int64_t target = status.committed_index;

      if (!fsm_->WaitApplied(target, /*timeout_ms=*/50)) {
        LOG(WARNING) << "Lease Scan: applied index 未追上 committed=" << target
                     << " applied=" << fsm_->LastAppliedIndex() << "，降级读";
      }
      auto kvs = fsm_->Scan(request->start_key(), request->end_key(),
                            request->limit());
      response->set_success(true);
      for (const auto &[k, v] : kvs) {
        auto *pair = response->add_kvs();
        pair->set_key(k);
        pair->set_value(v);
      }
      done->Run();
      return;
    }
    // 降级到 Log Read
    // ── 线性一致读：将 OP_SCAN 提交 Raft 日志，在 on_apply 后读状态机 ──
    kv::KvOperation op;
    op.set_op(kv::OP_SCAN);
    op.set_key(request->start_key()); // key 复用为 start_key
    op.set_value(request->end_key()); // value 复用为 end_key

    butil::IOBuf log;
    butil::IOBufAsZeroCopyOutputStream stream(&log);
    if (!op.SerializeToZeroCopyStream(&stream)) {
      response->set_success(false);
      response->set_error("序列化失败");
      done->Run();
      return;
    }
    braft::Task task;
    task.data = &log;
    task.done = new ScanReadIndexClosure(request, response, done, fsm_);
    task.expected_term = fsm_->LeaderTerm();
    node_->apply(task);
    return;
  }
  // ── 弱一致读：直接读状态机 ──
  auto kvs =
      fsm_->Scan(request->start_key(), request->end_key(), request->limit());
  response->set_success(true);
  for (const auto &[key, value] : kvs) {
    auto *pair = response->add_kvs();
    pair->set_key(key);
    pair->set_value(value);
  }
  done->Run();
}
#endif
// ── Scan RPC：snapshot_ts > 0 自动按线性一致读处理 ───────────────────
void KVServiceImpl::Scan(::google::protobuf::RpcController * /*controller*/,
                         const kv::ScanRequest *request,
                         kv::ScanResponse *response,
                         ::google::protobuf::Closure *done) {
  const bool need_barrier =
      request->linearizable() || request->snapshot_ts() > 0;
  // 保持原 Scan 行为：无论强读还是弱读，都只由 leader 提供服务。
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (!need_barrier) {
    // snapshot_ts == 0 的旧弱读路径，仍读取 data CF。
    FillScanResponse(*request, response, fsm_);
    done->Run();
    return;
  }
  // 同 Get：只有状态机确认自己是 leader（已应用完前任的全部日志）时才允许
  // lease read，否则退化到 Log Read，由日志顺序建立读屏障。
  if (fsm_->IsLeader() && node_->is_leader_lease_valid()) {
    braft::NodeStatus status;
    node_->get_status(&status);

    const int64_t target = status.committed_index;

    if (fsm_->WaitApplied(target, 50)) {
      FillScanResponse(*request, response, fsm_);
      done->Run();
      return;
    }
    LOG(WARNING) << "Lease Scan: applied index 未追上 committed=" << target
                 << " applied=" << fsm_->LastAppliedIndex()
                 << "，回退到 Log Read";
  }
  // 租约无效或 applied barrier 超时：通过 OP_SCAN 建立 Raft 顺序点。
  kv::KvOperation op;
  op.set_op(kv::OP_SCAN);
  op.set_key(request->start_key());
  op.set_value(request->end_key());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }
  braft::Task task;
  task.data = &log;
  task.done = new ScanReadIndexClosure(request, response, done, fsm_);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}



// ── TxnPrewrite RPC：2PC 第一阶段，需经过 Raft 共识 ──────────────────
void KVServiceImpl::TxnPrewrite(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::TxnPrewriteRequest *request, kv::TxnPrewriteResponse *response,
    ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->mutations().empty() || request->primary().empty() ||
      request->start_ts() == 0) {
    response->set_success(false);
    response->set_error("非法 prewrite 请求：mutations/primary/start_ts 缺失");
    done->Run();
    return;
  }
  ApplyTxnOp(kv::OP_TXN_PREWRITE, request->SerializeAsString(), response, done);
}

// ── TxnCommit RPC：2PC 第二阶段，需经过 Raft 共识 ────────────────────
void KVServiceImpl::TxnCommit(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::TxnCommitRequest *request, kv::TxnCommitResponse *response,
    ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->keys().empty() || request->start_ts() == 0) {
    response->set_success(false);
    response->set_error("非法 commit 请求：keys/start_ts 缺失");
    done->Run();
    return;
  }
  // commit_ts == 0 → 由 leader 的 LocalTso 分配（预留上界内），
  // 写进日志后各副本确定性一致
  kv::TxnCommitRequest req = *request;
  if (req.commit_ts() == 0) {
    AllocateTso(1, 0,
                [this, req = std::move(req), response,
                 done](const butil::Status &s, uint64_t ts) mutable {
                  if (!s.ok()) {
                    response->set_success(false);
                    response->set_error(s.error_str());
                    done->Run();
                    return;
                  }
                  // TSO 发出的号严格递增，ts ≤ start_ts 说明 start_ts
                  // 不是本集群发出的（超前伪造）。不能用 start_ts+1 兜底：
                  // 那会突破预留上界，破坏「已发出的号 ≤ 已持久化上界」
                  // 的 failover 不变式。直接拒绝。
                  if (ts <= req.start_ts()) {
                    response->set_success(false);
                    response->set_error(
                        "非法 commit 请求：start_ts 超前于本集群 TSO");
                    done->Run();
                    return;
                  }
                  req.set_commit_ts(ts);
                  ApplyTxnOp(kv::OP_TXN_COMMIT, req.SerializeAsString(),
                             response, done);
                });
    return;
  }
  if (req.commit_ts() <= req.start_ts()) {
    response->set_success(false);
    response->set_error("非法 commit 请求：commit_ts 必须大于 start_ts");
    done->Run();
    return;
  }
  ApplyTxnOp(kv::OP_TXN_COMMIT, req.SerializeAsString(), response, done);
}

// ── TxnRollback RPC：清锁 + 写回滚标记，需经过 Raft 共识 ─────────────
void KVServiceImpl::TxnRollback(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::TxnRollbackRequest *request, kv::TxnRollbackResponse *response,
    ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->keys().empty() || request->start_ts() == 0) {
    response->set_success(false);
    response->set_error("非法 rollback 请求：keys/start_ts 缺失");
    done->Run();
    return;
  }
  ApplyTxnOp(kv::OP_TXN_ROLLBACK, request->SerializeAsString(), response, done);
}

// ── CheckTxnStatus RPC：查/清 primary 残锁，可能改状态机，走 Raft ────
void KVServiceImpl::CheckTxnStatus(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::CheckTxnStatusRequest *request,
    kv::CheckTxnStatusResponse *response, ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->primary().empty() || request->lock_ts() == 0) {
    response->set_success(false);
    response->set_error("非法 check 请求：primary/lock_ts 缺失");
    done->Run();
    return;
  }
  // now_ts 由 leader 定好并随日志复制（复用 key 字段），
  // 保证 TTL 判定在各副本确定性一致
  ApplyTxnOp(kv::OP_TXN_RESOLVE, request->SerializeAsString(), response, done,
             std::to_string(HybridNowTs()));
}

} // namespace raftkv