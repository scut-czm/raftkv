// tests/integration/txn_probe.cc
// 事务探针：Channel 直连 --addr，不做自动 redirect 跟随（redirect 本身就是被测行为）。
// 输出 machine-readable 单行结果，供 txn_failover_test.sh 用 grep 断言：
//   prewrite/commit/rollback → "success=0|1 error=... redirect=..."
//   get                      → "success=0|1 found=0|1 value=... locked=0|1 redirect=..."
//   check(CheckTxnStatus)    → "success=0|1 committed=0|1 commit_ts=... ttl=... redirect=..."
//   tso                      → "success=0|1 ts=... count=... redirect=... error=..."
//
// 用法示例：
//   ./txn_probe --addr=127.0.0.1:8200 --op=prewrite --key=k --value=v --primary=k --start_ts=26214400
//   ./txn_probe --addr=127.0.0.1:8200 --op=commit --key=k --start_ts=26214400 --commit_ts=52428800
//   ./txn_probe --addr=127.0.0.1:8200 --op=get --key=k --snapshot_ts=78643200
//   ./txn_probe --addr=127.0.0.1:8200 --op=rollback --key=k --start_ts=26214400
//   ./txn_probe --addr=127.0.0.1:8200 --op=check --primary=k --lock_ts=26214400
//   ./txn_probe --addr=127.0.0.1:8200 --op=tso --count=10

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <cstdio>
#include <string>

#include "kv.pb.h"

DEFINE_string(addr, "127.0.0.1:8200", "目标节点地址 ip:port（直连，不跟随 redirect）");
DEFINE_string(op, "get", "prewrite | commit | rollback | get | check | tso");
DEFINE_string(key, "", "键");
DEFINE_string(value, "", "值（prewrite 用）");
DEFINE_string(primary, "", "primary key（prewrite/check 用，默认取 --key）");
DEFINE_uint64(start_ts, 0, "事务 start_ts");
DEFINE_uint64(commit_ts, 0, "commit_ts，0 = 由 leader 分配");
DEFINE_uint64(snapshot_ts, 0, "快照读 ts（get 用）");
DEFINE_uint64(lock_ts, 0, "锁 ts（check 用，默认取 --start_ts）");
DEFINE_uint64(ttl_ms, 3000, "prewrite 锁 TTL");
DEFINE_bool(is_delete, false, "prewrite 时 mutation 为 DELETE");
DEFINE_uint32(count, 1, "tso 批量取号个数，返回 [ts, ts+count)");
DEFINE_int32(timeout_ms, 1000, "RPC 超时");

namespace {

// RPC 层失败（连接拒绝/超时等）统一输出 rpc_error，退出码 2，方便脚本区分。
int RpcFail(const brpc::Controller &cntl) {
  printf("success=0 rpc_error=%s\n", cntl.ErrorText().c_str());
  return 2;
}

int DoPrewrite(kv::KvService_Stub &stub) {
  kv::TxnPrewriteRequest req;
  auto *m = req.add_mutations();
  m->set_op(FLAGS_is_delete ? kv::Mutation::DELETE : kv::Mutation::PUT);
  m->set_key(FLAGS_key);
  m->set_value(FLAGS_value);
  req.set_primary(FLAGS_primary.empty() ? FLAGS_key : FLAGS_primary);
  req.set_start_ts(FLAGS_start_ts);
  req.set_ttl_ms(FLAGS_ttl_ms);

  kv::TxnPrewriteResponse resp;
  brpc::Controller cntl;
  stub.TxnPrewrite(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  printf("success=%d error=%s redirect=%s conflict_commit_ts=%llu\n",
         resp.success() ? 1 : 0, resp.error().c_str(), resp.redirect().c_str(),
         (unsigned long long)resp.conflict_commit_ts());
  return resp.success() ? 0 : 1;
}

int DoCommit(kv::KvService_Stub &stub) {
  kv::TxnCommitRequest req;
  req.add_keys(FLAGS_key);
  req.set_start_ts(FLAGS_start_ts);
  req.set_commit_ts(FLAGS_commit_ts);

  kv::TxnCommitResponse resp;
  brpc::Controller cntl;
  stub.TxnCommit(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  printf("success=%d error=%s redirect=%s commit_ts=%llu\n",
         resp.success() ? 1 : 0, resp.error().c_str(), resp.redirect().c_str(),
         (unsigned long long)resp.commit_ts());
  return resp.success() ? 0 : 1;
}

int DoRollback(kv::KvService_Stub &stub) {
  kv::TxnRollbackRequest req;
  req.add_keys(FLAGS_key);
  req.set_start_ts(FLAGS_start_ts);

  kv::TxnRollbackResponse resp;
  brpc::Controller cntl;
  stub.TxnRollback(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  printf("success=%d error=%s redirect=%s\n",
         resp.success() ? 1 : 0, resp.error().c_str(), resp.redirect().c_str());
  return resp.success() ? 0 : 1;
}

int DoGet(kv::KvService_Stub &stub) {
  kv::GetRequest req;
  req.set_key(FLAGS_key);
  req.set_snapshot_ts(FLAGS_snapshot_ts);

  kv::GetResponse resp;
  brpc::Controller cntl;
  stub.Get(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  const bool locked = resp.has_lock_conflict();
  printf("success=%d found=%d value=%s locked=%d lock_ts=%llu redirect=%s error=%s\n",
         resp.success() ? 1 : 0, resp.found() ? 1 : 0, resp.value().c_str(),
         locked ? 1 : 0,
         (unsigned long long)(locked ? resp.lock_conflict().lock_ts() : 0),
         resp.redirect().c_str(), resp.error().c_str());
  return resp.success() ? 0 : 1;
}

int DoCheck(kv::KvService_Stub &stub) {
  kv::CheckTxnStatusRequest req;
  req.set_primary(FLAGS_primary.empty() ? FLAGS_key : FLAGS_primary);
  req.set_lock_ts(FLAGS_lock_ts != 0 ? FLAGS_lock_ts : FLAGS_start_ts);

  kv::CheckTxnStatusResponse resp;
  brpc::Controller cntl;
  stub.CheckTxnStatus(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  printf("success=%d committed=%d commit_ts=%llu ttl=%llu redirect=%s error=%s\n",
         resp.success() ? 1 : 0, resp.committed() ? 1 : 0,
         (unsigned long long)resp.commit_ts(),
         (unsigned long long)resp.remaining_ttl_ms(),
         resp.redirect().c_str(), resp.error().c_str());
  return resp.success() ? 0 : 1;
}

int DoTso(kv::KvService_Stub &stub) {
  kv::TsoRequest req;
  req.set_count(FLAGS_count);

  kv::TsoResponse resp;
  brpc::Controller cntl;
  stub.GetTso(&cntl, &req, &resp, nullptr);
  if (cntl.Failed()) return RpcFail(cntl);
  printf("success=%d ts=%llu count=%u redirect=%s error=%s\n",
         resp.success() ? 1 : 0, (unsigned long long)resp.ts(), FLAGS_count,
         resp.redirect().c_str(), resp.error().c_str());
  return resp.success() ? 0 : 1;
}

}  // namespace

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  brpc::ChannelOptions options;
  options.timeout_ms = FLAGS_timeout_ms;
  options.max_retry = 0;  // 不重试：探测语义要求一发一收

  brpc::Channel channel;
  if (channel.Init(FLAGS_addr.c_str(), &options) != 0) {
    printf("success=0 rpc_error=channel_init_failed addr=%s\n", FLAGS_addr.c_str());
    return 2;
  }
  kv::KvService_Stub stub(&channel);

  if (FLAGS_op == "prewrite") return DoPrewrite(stub);
  if (FLAGS_op == "commit")   return DoCommit(stub);
  if (FLAGS_op == "rollback") return DoRollback(stub);
  if (FLAGS_op == "get")      return DoGet(stub);
  if (FLAGS_op == "check")    return DoCheck(stub);
  if (FLAGS_op == "tso")      return DoTso(stub);

  printf("success=0 rpc_error=unknown_op op=%s\n", FLAGS_op.c_str());
  return 2;
}
