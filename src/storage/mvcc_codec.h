#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace raftkv {
// MVCC 键编码：user_key + 8 字节大端序的 ~ts（按位取反）。
// 性质：同一 user_key 下，ts 越大编码后字典序越小 → Seek(key, ts) 一次命中
// 「commit_ts <= ts 的最新版本」，这是整个 MVCC 读路径的基石。

class MvccCodec {
public:
  static constexpr size_t kTsLen = 8;

  static std::string EncodeKey(std::string_view user_key, uint64_t ts) {
    std::string out;
    out.reserve(user_key.size() + kTsLen);
    out.append(user_key);
    // TODO: 实现大端序 ~ts 编码
    AppendInvertedTs(&out, ts);
    return out;
  }

  static bool DecodeKey(std::string_view mvcc_key, std::string_view *user_key,
                        uint64_t *ts) {
    if (mvcc_key.size() < kTsLen) {
      return false;
    }
    *user_key = mvcc_key.substr(0, mvcc_key.size() - kTsLen);
    uint64_t inv = 0;
    for (size_t i = mvcc_key.size() - kTsLen; i < mvcc_key.size(); i++) {
      inv = (inv << 8) | static_cast<uint8_t>(mvcc_key[i]);
    }
    *ts = ~inv;
    return true;
  }

  // 该 user_key 全部版本的扫描上界（不含）：user_key 后追加 0xFF...
  // 因为 ~ts 编码后 ts=UINT64_MAX 是全 0，ts=0 是全 0xFF。
  static std::string VersionRangeEnd(std::string_view user_key) {
    std::string out(user_key);
    out.append(kTsLen, '\xff');
    out.push_back('\x00');
    return out;
  }

private:
  static void AppendInvertedTs(std::string *out, uint64_t ts) {
    uint64_t inv = ~ts;
    for (int i = 7; i >= 0; --i) {
      out->push_back(static_cast<char>((inv >> (i * 8)) & 0xff));
    }
  }
};

// lock CF 的值：未提交事务的锁记录。
// 用紧凑二进制而不是 protobuf，减少一个 .proto 依赖（也可换成 proto）。
struct LockInfo {
  enum Type : uint8_t { kPut = 0, kDelete = 1 };

  uint64_t start_ts = 0;
  uint64_t ttl_ms = 0;
  Type lock_type = kPut;
  std::string primary;

  std::string Serialize() const;
  static std::optional<LockInfo> Deserialize(std::string_view data);
};

// write CF 的值：已提交（或已回滚）的写记录。
struct WriteInfo {
  enum Kind : uint8_t { kPut = 0, kDelete = 1, kRollback = 2 };

  uint64_t start_ts = 0; // 指向 default CF 中的数据版本
  Kind kind = kPut;

  std::string Serialize() const;
  static std::optional<WriteInfo> Deserialize(std::string_view data);
};

} // namespace raftkv