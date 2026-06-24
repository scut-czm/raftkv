// TODO: 声明 RaftKvClientAdapter : KvClientInterface
//       桥接 raftkv::KVClient* → KvClientInterface
//       Put / Get / Delete / Scan / BatchPut
//       private: client_（raftkv::KVClient*）

#pragma once

#include <string>
#include <vector>

#include "src/client/kv_client.h"
#include "src/sql/kv_client_interface.h"

namespace raftsql {
// 将 raftkv::KVClient 适配为 KvClientInterface
// 使 SQLExecutor 可驱动真实 RaftKV 集群
class RaftKvClientAdapter : public KvClientInterface {
public:
  explicit RaftKvClientAdapter(raftkv::KVClient *client);

  bool Put(const std::string &key, const std::string &value) override;
  std::string Get(const std::string &key) override;
  bool Delete(const std::string &key) override;
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit = 10000) override;
  bool BatchPut(
      const std::vector<std::pair<std::string, std::string>> &kvs) override;

private:
  raftkv::KVClient *client_;
};
} // namespace raftsql