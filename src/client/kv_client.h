#pragma once

#include "kv.pb.h"

#include <brpc/channel.h>
#include <memory>
#include <string>
#include <vector>

namespace raftkv {

struct ClientOptions {
  std::string peers; // 逗号分隔的节点列表，如 "127.0.0.1:8200,127.0.0.1:8201"
  int max_retry = 50; // redirect 最大重试次数（含 no leader 等待）
  int timeout_ms = 3000;    // 单次 RPC 超时（毫秒）
  bool linearizable = true; // 默认使用线性一致读
};

// 事务写操作（Prewrite 的一条 mutation）
struct TxnMutation {
  std::string key;
  std::string value;
  bool is_delete = false;
};

// 快照读遇到的未提交锁信息（lock_ts != 0 表示有锁）
struct LockInfo {
  std::string key;
  std::string primary;
  uint64_t lock_ts = 0;
};

// CheckTxnStatus 的判定结果
struct TxnStatus {
  bool committed = false;        // true → commit_ts 有效
  uint64_t commit_ts = 0;
  uint64_t remaining_ttl_ms = 0; // 未提交时：0 = 已回滚，>0 = 锁仍存活
};

class KVClient {
public:
  explicit KVClient(const ClientOptions &options);
  explicit KVClient(const std::string &server_addr);

  bool Put(const std::string &key, const std::string &value);
  bool Get(const std::string &key, std::string *value_out, bool *found_out);
  bool Delete(const std::string &key);
  bool Scan(const std::string &start_key, const std::string &end_key, int limit,
            std::vector<std::pair<std::string, std::string>> *kvs_out);

  // ── 事务接口 ─────────────────────────────────────────────────────────
  // 取号：返回号段起点，独占 [ts, ts+count)；0 表示失败
  uint64_t GetTso(uint32_t count = 1);

  // 2PC 第一阶段：给所有 mutations 上锁。冲突时返回 false，
  // conflict_out（可为 nullptr）带回冲突锁信息供 resolve。
  bool TxnPrewrite(const std::vector<TxnMutation> &mutations,
                   const std::string &primary, uint64_t start_ts,
                   uint64_t ttl_ms, LockInfo *conflict_out, std::string *err);

  // 2PC 第二阶段：commit_ts 传 0 由 leader TSO 分配，
  // 实际使用值经 commit_ts_out（可为 nullptr）带回。
  bool TxnCommit(const std::vector<std::string> &keys, uint64_t start_ts,
                 uint64_t commit_ts, uint64_t *commit_ts_out,
                 std::string *err);

  bool TxnRollback(const std::vector<std::string> &keys, uint64_t start_ts,
                   std::string *err);

  // 查询/推进事务状态（读到残锁时用 primary + lock_ts 调用）
  bool CheckTxnStatus(const std::string &primary, uint64_t lock_ts,
                      TxnStatus *status_out, std::string *err);

  // MVCC 快照读：只看 commit_ts <= snapshot_ts 的版本。
  // 返回 false 且 lock_out->lock_ts != 0 表示撞到未提交锁。
  bool SnapshotGet(const std::string &key, uint64_t snapshot_ts,
                   std::string *value_out, bool *found_out, LockInfo *lock_out,
                   std::string *err);

  bool SnapshotScan(const std::string &start_key, const std::string &end_key,
                    int limit, uint64_t snapshot_ts,
                    std::vector<std::pair<std::string, std::string>> *kvs_out,
                    LockInfo *lock_out, std::string *err);

  // 解析 peers 字符串为地址列表
  static std::vector<std::string> ParsePeers(const std::string &peers);

private:
  // 初始化 channel 连接到指定地址
  bool InitChannel(const std::string &addr);

  // 处理 redirect：切换 channel 到新地址，返回 true 表示应重试
  bool HandleRedirect(const std::string &redirect_addr);

  ClientOptions options_;
  std::vector<std::string> peer_list_; // 所有节点地址
  std::string current_addr_;           // 当前连接的地址
  brpc::Channel channel_;
  std::unique_ptr<kv::KvService_Stub> stub_;
};

} // namespace raftkv
