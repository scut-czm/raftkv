// TODO: Put —— client_->Put(key, value)
// TODO: Get —— client_->Get(key, &value, &found)，未找到返回空串
// TODO: Delete —— client_->Delete(key)
// TODO: Scan —— client_->Scan(start, end, limit, &results)
// TODO: BatchPut —— 逐条调用 client_->Put

#include "src/sql/raft_kv_client_adapter.h"

namespace raftsql {
RaftKvClientAdapter::RaftKvClientAdapter(raftkv::KVClient *client)
    : client_(client) {}

bool RaftKvClientAdapter::Put(const std::string &key,
                              const std::string &value) {
  return client_->Put(key, value);
}

bool RaftKvClientAdapter::Delete(const std::string &key) {
  return client_->Delete(key);
}

std::string RaftKvClientAdapter::Get(const std::string &key) {
  std::string value;
  bool found = false;
  if (client_->Get(key, &value, &found) && found) {
    return value;
  }
  return "";
}

std::vector<std::pair<std::string, std::string>>
RaftKvClientAdapter::Scan(const std::string &start_key,
                          const std::string &end_key, int limit) {
  std::vector<std::pair<std::string, std::string>> results;
  client_->Scan(start_key, end_key, limit, &results);

  return results;
}


bool RaftKvClientAdapter::BatchPut(
    const std::vector<std::pair<std::string, std::string>>& kvs) {
  for (const auto& [key, value] : kvs) {
    if (!client_->Put(key, value)) return false;
  }
  return true;
}



} // namespace raftsql