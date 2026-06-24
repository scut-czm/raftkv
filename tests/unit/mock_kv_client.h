// TODO: 实现 MockKvClient : KvClientInterface
//       使用 std::map<string,string> 作内存存储
//       Put / Get / Delete / Scan(lower_bound迭代) / BatchPut
//       辅助方法：Clear() / Size() / Data()

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "src/sql/kv_client_interface.h"

namespace raftsql {
class MockKvClient : public KvClientInterface {
public:
  bool Put(const std::string &key, const std::string &value) override {
    data_[key] = value;
    return true;
  }

  std::string Get(const std::string &key) override {
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : "";
  }

  bool Delete(const std::string &key) override {
    data_.erase(key);
    return true;
  }
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit = 10000) override {
    std::vector<std::pair<std::string, std::string>> results;
    auto it = data_.lower_bound(start_key);
    for (; it != data_.end() && it->first < end_key; ++it) {
      results.emplace_back(it->first, it->second);
      if (static_cast<int>(results.size()) >= limit)
        break;
    }
    return results;
  }

  bool BatchPut(
      const std::vector<std::pair<std::string, std::string>> &kvs) override {
    for (const auto &[key, value] : kvs) {
      data_[key] = value;
    }
    return true;
  }

  void Clear() { data_.clear(); }
  size_t Size() const { return data_.size(); }

  const std::map<std::string, std::string> &Data() const { return data_; }

private:
  std::map<std::string, std::string> data_;
};

} // namespace raftsql