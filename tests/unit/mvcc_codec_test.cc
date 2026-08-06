#include "storage/mvcc_codec.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace raftkv {
namespace {

// 同一 user_key 下，ts 越大编码后字典序越小（新版本排前面）。
TEST(MvccCodecTest, NewerVersionSortsFirst) {
  const std::string k9 = MvccCodec::EncodeKey("key", 9);
  const std::string k5 = MvccCodec::EncodeKey("key", 5);
  const std::string k3 = MvccCodec::EncodeKey("key", 3);
  EXPECT_LT(k9, k5);
  EXPECT_LT(k5, k3);

  // 大跨度也成立
  EXPECT_LT(MvccCodec::EncodeKey("key", UINT64_MAX),
            MvccCodec::EncodeKey("key", 0));
}

TEST(MvccCodecTest, EncodeDecodeRoundTrip) {
  const uint64_t cases[] = {0, 1, 5, 255, 256, 0x0102030405060708ULL,
                            UINT64_MAX - 1, UINT64_MAX};
  for (uint64_t ts : cases) {
    const std::string encoded = MvccCodec::EncodeKey("user_key", ts);
    ASSERT_EQ(encoded.size(), 8 + MvccCodec::kTsLen);

    std::string_view user_key;
    uint64_t decoded_ts = 0;
    ASSERT_TRUE(MvccCodec::DecodeKey(encoded, &user_key, &decoded_ts));
    EXPECT_EQ(user_key, "user_key");
    EXPECT_EQ(decoded_ts, ts);
  }
}

TEST(MvccCodecTest, EncodeDecodeEmptyUserKey) {
  const std::string encoded = MvccCodec::EncodeKey("", 42);
  ASSERT_EQ(encoded.size(), MvccCodec::kTsLen);

  std::string_view user_key;
  uint64_t ts = 0;
  ASSERT_TRUE(MvccCodec::DecodeKey(encoded, &user_key, &ts));
  EXPECT_TRUE(user_key.empty());
  EXPECT_EQ(ts, 42u);
}

TEST(MvccCodecTest, DecodeKeyTooShortFails) {
  std::string_view user_key;
  uint64_t ts = 0;
  EXPECT_FALSE(MvccCodec::DecodeKey("short", &user_key, &ts));
  EXPECT_FALSE(MvccCodec::DecodeKey("", &user_key, &ts));
}

// 扫描上界严格大于该 key 的全部版本键（包括 ts=0 的最大版本键）。
TEST(MvccCodecTest, VersionRangeEndCoversAllVersions) {
  const std::string end = MvccCodec::VersionRangeEnd("key");
  EXPECT_LT(MvccCodec::EncodeKey("key", 0), end);          // 最旧
  EXPECT_LT(MvccCodec::EncodeKey("key", 5), end);
  EXPECT_LT(MvccCodec::EncodeKey("key", UINT64_MAX), end); // 最新
}

TEST(LockInfoTest, SerializeDeserializeRoundTrip) {
  LockInfo lock;
  lock.start_ts = 0x0102030405060708ULL;
  lock.ttl_ms = 3000;
  lock.lock_type = LockInfo::kDelete;
  lock.primary = "primary_key";

  auto decoded = LockInfo::Deserialize(lock.Serialize());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->start_ts, lock.start_ts);
  EXPECT_EQ(decoded->ttl_ms, lock.ttl_ms);
  EXPECT_EQ(decoded->lock_type, lock.lock_type);
  EXPECT_EQ(decoded->primary, lock.primary);
}

TEST(LockInfoTest, EmptyPrimaryRoundTrip) {
  LockInfo lock;
  lock.start_ts = 7;
  lock.ttl_ms = 0;
  lock.lock_type = LockInfo::kPut;
  lock.primary = "";

  auto decoded = LockInfo::Deserialize(lock.Serialize());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->start_ts, 7u);
  EXPECT_EQ(decoded->ttl_ms, 0u);
  EXPECT_EQ(decoded->lock_type, LockInfo::kPut);
  EXPECT_TRUE(decoded->primary.empty());
}

TEST(LockInfoTest, BoundaryTimestamps) {
  for (uint64_t ts : {uint64_t{0}, UINT64_MAX}) {
    LockInfo lock;
    lock.start_ts = ts;
    lock.ttl_ms = ts;
    auto decoded = LockInfo::Deserialize(lock.Serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->start_ts, ts);
    EXPECT_EQ(decoded->ttl_ms, ts);
  }
}

TEST(LockInfoTest, DeserializeBadDataFails) {
  EXPECT_FALSE(LockInfo::Deserialize("").has_value());
  EXPECT_FALSE(LockInfo::Deserialize("1234567").has_value());   // < 8 字节
  EXPECT_FALSE(LockInfo::Deserialize("12345678").has_value());  // 缺 ttl
  EXPECT_FALSE(
      LockInfo::Deserialize("1234567812345678").has_value());   // 缺 type
}

TEST(WriteInfoTest, SerializeDeserializeRoundTrip) {
  for (auto kind : {WriteInfo::kPut, WriteInfo::kDelete, WriteInfo::kRollback}) {
    WriteInfo write;
    write.start_ts = 0xDEADBEEFCAFEBABEULL;
    write.kind = kind;

    auto decoded = WriteInfo::Deserialize(write.Serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->start_ts, write.start_ts);
    EXPECT_EQ(decoded->kind, kind);
  }
}

TEST(WriteInfoTest, BoundaryTimestamps) {
  for (uint64_t ts : {uint64_t{0}, UINT64_MAX}) {
    WriteInfo write;
    write.start_ts = ts;
    auto decoded = WriteInfo::Deserialize(write.Serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->start_ts, ts);
  }
}

TEST(WriteInfoTest, DeserializeBadDataFails) {
  EXPECT_FALSE(WriteInfo::Deserialize("").has_value());
  EXPECT_FALSE(WriteInfo::Deserialize("1234567").has_value());  // < 8 字节
  EXPECT_FALSE(WriteInfo::Deserialize("12345678").has_value()); // 缺 kind
}

} // namespace
} // namespace raftkv
