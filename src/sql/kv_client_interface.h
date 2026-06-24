// TODO: 定义 KvClientInterface 抽象接口
//       virtual Put(key, value) / Get(key) / Delete(key) / Scan(start, end,
//       limit) virtual BatchPut(kvs) —— 默认实现逐条调用 Put
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace raftsql {

// KV 客户端抽象接口
// MockKvClient 和 RaftKvClientAdapter 均实现此接口
class KvClientInterface {
public:
  virtual ~KvClientInterface() = default;

  // 写入（走 Raft 共识）
  virtual bool Put(const std::string &key, const std::string &value) = 0;
  // 读取（可配置线性一致读）
  virtual std::string Get(const std::string &key) = 0;
  // 删除（走 Raft 共识）
  virtual bool Delete(const std::string &key) = 0;

  // 范围扫描 [start_key, end_key)
  virtual std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit = 10000) = 0;
  // 批量写入（减少 Raft 写放大，Day 14 性能优化）
  virtual bool
  BatchPut(const std::vector<std::pair<std::string, std::string>> &kvs) {
    for (const auto &[key, value] : kvs) {
      if (!Put(key, value)) {
        return false;
      }
    }
    return true;
  }
};

} // namespace raftsql