#pragma once

#include "kv.pb.h"
#include "raft/kv_state_machine.h"

#include <braft/raft.h>
#include <butil/logging.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include <functional>

namespace raftkv {

// 根据 snapshot_ts 选择旧 data CF 或 MVCC 快照读，并填充 RPC 响应。
void FillGetResponse(const kv::GetRequest &request, kv::GetResponse *response,
                     KVStateMachine *fsm);

void FillScanResponse(const kv::ScanRequest &request,
                      kv::ScanResponse *response, KVStateMachine *fsm);

// 泛型 Closure：统一处理 Put/Delete 的异步回调
template <typename ResponseType> class KVClosure : public braft::Closure {
public:
  KVClosure(ResponseType *response, google::protobuf::Closure *done)
      : response_(response), done_(done) {}

  void Run() override {
    if (status().ok()) {
      response_->set_success(true);
    } else {
      response_->set_success(false);
      response_->set_error(status().error_cstr());
      LOG(WARNING) << "操作失败: " << status().error_cstr();
    }
    done_->Run();
    delete this;
  }

private:
  ResponseType *response_;
  google::protobuf::Closure *done_;
};

// ── 事务 RPC 泛型 Closure ────────────────────────────────────────────
// 与 KVClosure 的区别：暴露 response()，成功路径的业务结果
// （conflict_*, commit_ts, committed 等）由 on_apply 直接写入 response；
// Run() 只兜底 raft 层失败（丢 leader / 超时 / term 变更）。
template <typename ResponseType> class TxnClosure : public braft::Closure {
public:
  TxnClosure(ResponseType *response, google::protobuf::Closure *done)
      : response_(response), done_(done) {}

  ResponseType *response() { return response_; }

  void Run() override {
    if (!status().ok()) {
      response_->set_success(false);
      response_->set_error(status().error_str());
      LOG(WARNING) << "事务操作 raft 失败: " << status().error_cstr();
    }
    // status().ok() 时，success 与结果字段已由 on_apply 填好，这里不再覆盖
    done_->Run();
    delete this;
  }

private:
  ResponseType *response_;
  google::protobuf::Closure *done_;
};

// TSO 预留日志的 Closure：apply 成功后回调继续发号（先持久化、后发号）
class TsoReserveClosure : public braft::Closure {
public:
  explicit TsoReserveClosure(std::function<void(const butil::Status &)> cb)
      : cb_(std::move(cb)) {}

  void Run() override {
    cb_(status());
    delete this;
  }

private:
  std::function<void(const butil::Status &)> cb_;
};

// ReadIndex 线性一致读 Closure
// braft 确认 commit index 后回调，此时读状态机保证线性一致
class ReadIndexClosure : public braft::Closure {
public:
  ReadIndexClosure(const kv::GetRequest *req, kv::GetResponse *resp,
                   google::protobuf::Closure *done, KVStateMachine *fsm)
      : req_(req), resp_(resp), done_(done), fsm_(fsm) {}

  void Run() override {
    if (status().ok()) {
      // std::string value;
      // bool found = fsm_->Get(req_->key(), &value);
      // resp_->set_success(true);
      // resp_->set_found(found);
      // if (found) {
      //   resp_->set_value(value);
      // }
      FillGetResponse(*req_, resp_, fsm_);
    } else {
      resp_->set_success(false);
      resp_->set_error(status().error_str());
    }
    done_->Run();
    delete this;
  }

private:
  const kv::GetRequest *req_;
  kv::GetResponse *resp_;
  google::protobuf::Closure *done_;
  KVStateMachine *fsm_;
};

// Scan ReadIndex 线性一致读 Closure
class ScanReadIndexClosure : public braft::Closure {
public:
  ScanReadIndexClosure(const kv::ScanRequest *req, kv::ScanResponse *resp,
                       google::protobuf::Closure *done, KVStateMachine *fsm)
      : req_(req), resp_(resp), done_(done), fsm_(fsm) {}

  void Run() override {
    if (status().ok()) {
      // auto kvs = fsm_->Scan(req_->start_key(), req_->end_key(),
      // req_->limit()); resp_->set_success(true); for (const auto &[k, v] :
      // kvs) {
      //   auto *pair = resp_->add_kvs();
      //   pair->set_key(k);
      //   pair->set_value(v);
      // }
      FillScanResponse(*req_, resp_, fsm_);
    } else {
      resp_->set_success(false);
      resp_->set_error(status().error_str());
    }
    done_->Run();
    delete this;
  }

private:
  const kv::ScanRequest *req_;
  kv::ScanResponse *resp_;
  google::protobuf::Closure *done_;
  KVStateMachine *fsm_;
};

// KV RPC 服务实现（对应 proto 中的 KvService）
class KVServiceImpl : public kv::KvService {
public:
  explicit KVServiceImpl(braft::Node *node, KVStateMachine *fsm)
      : node_(node), fsm_(fsm) {}

  void Put(::google::protobuf::RpcController *controller,
           const kv::PutRequest *request, kv::PutResponse *response,
           ::google::protobuf::Closure *done) override;
  void Get(::google::protobuf::RpcController *controller,
           const kv::GetRequest *request, kv::GetResponse *response,
           ::google::protobuf::Closure *done) override;

  void Delete(::google::protobuf::RpcController *controller,
              const kv::DeleteRequest *request, kv::DeleteResponse *response,
              ::google::protobuf::Closure *done) override;

  void Scan(::google::protobuf::RpcController *controller,
            const kv::ScanRequest *request, kv::ScanResponse *response,
            ::google::protobuf::Closure *done) override;

  // ── MVCC 事务 RPC ──
  void TxnPrewrite(::google::protobuf::RpcController *controller,
                   const kv::TxnPrewriteRequest *request,
                   kv::TxnPrewriteResponse *response,
                   ::google::protobuf::Closure *done) override;

  void TxnCommit(::google::protobuf::RpcController *controller,
                 const kv::TxnCommitRequest *request,
                 kv::TxnCommitResponse *response,
                 ::google::protobuf::Closure *done) override;

  void TxnRollback(::google::protobuf::RpcController *controller,
                   const kv::TxnRollbackRequest *request,
                   kv::TxnRollbackResponse *response,
                   ::google::protobuf::Closure *done) override;

  void CheckTxnStatus(::google::protobuf::RpcController *controller,
                      const kv::CheckTxnStatusRequest *request,
                      kv::CheckTxnStatusResponse *response,
                      ::google::protobuf::Closure *done) override;

  void GetTso(::google::protobuf::RpcController *controller,
              const kv::TsoRequest *request, kv::TsoResponse *response,
              ::google::protobuf::Closure *done) override;

private:
  // 将写操作序列化并提交给 Raft
  void ApplyWriteOp(const kv::KvOperation &op,
                    google::protobuf::Message *response,
                    google::protobuf::Closure *done);
  // 检查 Leader，非 Leader 则填充 redirect 并返回 true（表示已处理）
  template <typename ResponseType>
  bool RediretIfNotLeader(ResponseType *response,
                          google::protobuf::Closure *done);

  // 在已持久化的预留上界内分配 [start, start+count) 号段；
  // 上界不足时先提交 OP_TSO_RESERVE 日志，apply 后重试，
  // 成功时回调 cont(OK, start)。发号严格不超过已持久化上界。
  void AllocateTso(uint32_t count, int attempt,
                   std::function<void(const butil::Status &, uint64_t)> cont);

  // log_key：随日志复制的附加字段（如 OP_TXN_RESOLVE 的 now_ts）
  template <typename ResponseType>
  void ApplyTxnOp(kv::OperationType op_type, std::string req_payload,
                  ResponseType *response, google::protobuf::Closure *done,
                  std::string log_key = "");
  braft::Node *node_;
  KVStateMachine *fsm_;
};

} // namespace raftkv