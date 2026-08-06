#include "storage/mvcc_codec.h"

namespace raftkv {
namespace {

void PutU64(std::string *out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

bool GetU64(std::string_view *in, uint64_t *v) {
  if (in->size() < 8)
    return false;
  *v = 0;
  for (int i = 0; i < 8; ++i) {
    *v = (*v << 8) | static_cast<uint8_t>((*in)[i]);
  }
  in->remove_prefix(8);
  return true;
}
} // namespace

std::string LockInfo::Serialize() const {
  std::string out;
  PutU64(&out, start_ts);
  PutU64(&out, ttl_ms);
  out.push_back(static_cast<char>(lock_type));
  out.append(primary);
  return out;
}

std::optional<LockInfo> LockInfo::Deserialize(std::string_view data) {
  LockInfo info;
  if (!GetU64(&data, &info.start_ts)) {
    return std::nullopt;
  }
  if (!GetU64(&data, &info.ttl_ms))
    return std::nullopt;
  if (data.empty())
    return std::nullopt;
  info.lock_type = static_cast<Type>(data[0]);
  data.remove_prefix(1);
  info.primary.assign(data.data(), data.size());
  return info;
}

std::string WriteInfo::Serialize() const {
    std::string out;
    PutU64(&out, start_ts);
    out.push_back(static_cast<char>(kind));
    return out;
}
std::optional<WriteInfo> WriteInfo::Deserialize(std::string_view data) {
    WriteInfo info;
    if (!GetU64(&data, &info.start_ts)) {
        return std::nullopt;
    }
    if (data.empty()) {
        return std::nullopt;
    }
    info.kind=static_cast<Kind>(data[0]);
    return info;
}

} // namespace raftkv
