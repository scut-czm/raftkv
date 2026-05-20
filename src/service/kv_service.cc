#include "service/kv_service.h"
#include "braft/configuration.h"
#include "braft/raft.h"
#include "kv.pb.h"

#include <braft/util.h>
#include <butil/iobuf.h>
#include <butil/logging.h>
#include <sched.h>

namespace raftkv {
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
// ── Get RPC：支持弱一致读 / 线性一致读（走 Raft apply）───────────────
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

} // namespace raftkv