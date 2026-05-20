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

class KVClient {
public:
  explicit KVClient(const ClientOptions &options);
  explicit KVClient(const std::string &server_addr);

  bool Put(const std::string &key, const std::string &value);
  bool Get(const std::string &key, std::string *value_out, bool *found_out);
  bool Delete(const std::string &key);
  bool Scan(const std::string &start_key, const std::string &end_key, int limit,
            std::vector<std::pair<std::string, std::string>> *kvs_out);
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
