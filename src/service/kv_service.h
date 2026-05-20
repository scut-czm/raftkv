#pragma once

#include "kv.pb.h"
#include "raft/kv_state_machine.h"

#include <braft/raft.h>
#include <butil/logging.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

namespace raftkv {

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

// ReadIndex 线性一致读 Closure
// braft 确认 commit index 后回调，此时读状态机保证线性一致
class ReadIndexClosure : public braft::Closure {
public:
  ReadIndexClosure(const kv::GetRequest *req, kv::GetResponse *resp,
                   google::protobuf::Closure *done, KVStateMachine *fsm)
      : req_(req), resp_(resp), done_(done), fsm_(fsm) {}

  void Run() override {
    if (status().ok()) {
      std::string value;
      bool found = fsm_->Get(req_->key(), &value);
      resp_->set_success(true);
      resp_->set_found(found);
      if (found) {
        resp_->set_value(value);
      }
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
      auto kvs = fsm_->Scan(req_->start_key(), req_->end_key(), req_->limit());
      resp_->set_success(true);
      for (const auto &[k, v] : kvs) {
        auto *pair = resp_->add_kvs();
        pair->set_key(k);
        pair->set_value(v);
      }
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

private:
  // 将写操作序列化并提交给 Raft
  void ApplyWriteOp(const kv::KvOperation &op,
                    google::protobuf::Message *response,
                    google::protobuf::Closure *done);
  // 检查 Leader，非 Leader 则填充 redirect 并返回 true（表示已处理）
  template <typename ResponseType>
  bool RediretIfNotLeader(ResponseType *response,
                          google::protobuf::Closure *done);

  braft::Node *node_;
  KVStateMachine *fsm_;
};
} // namespace raftkv