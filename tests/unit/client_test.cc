#include "client/kv_client.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace raftkv {

// ── ParsePeers 单元测试（不需要集群）──────────────────────────────────

TEST(ClientUtilTest, ParsePeersBasic) {
  auto peers =
      KVClient::ParsePeers("127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202");
  ASSERT_EQ(peers.size(), 3u);
  EXPECT_EQ(peers[0], "127.0.0.1:8200");
  EXPECT_EQ(peers[1], "127.0.0.1:8201");
  EXPECT_EQ(peers[2], "127.0.0.1:8202");
}

TEST(ClientUtilTest, ParsePeersWithBraftSuffix) {
  // braft PeerId 格式: "ip:port:idx"，客户端需要去掉尾部 ":0"
  auto peers = KVClient::ParsePeers("127.0.0.1:8200:0,127.0.0.1:8201:0");
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0], "127.0.0.1:8200");
  EXPECT_EQ(peers[1], "127.0.0.1:8201");
}

TEST(ClientUtilTest, ParsePeersSingle) {
  auto peers = KVClient::ParsePeers("127.0.0.1:8200");
  ASSERT_EQ(peers.size(), 1u);
  EXPECT_EQ(peers[0], "127.0.0.1:8200");
}

TEST(ClientUtilTest, ParsePeersEmpty) {
  auto peers = KVClient::ParsePeers("");
  EXPECT_TRUE(peers.empty());
}
// ── ClientOptions 默认值 ──────────────────────────────────────────────

TEST(ClientOptionsTest, Defaults) {
  ClientOptions opts;
  EXPECT_EQ(opts.max_retry, 3);
  EXPECT_EQ(opts.timeout_ms, 3000);
  EXPECT_TRUE(opts.linearizable);
}
// ── 连接失败测试（不需要集群）─────────────────────────────────────────

TEST(ClientConnectionTest, ConnectToInvalidAddr) {
  ClientOptions opts;
  opts.peers = "127.0.0.1:19999"; // 不存在的端口
  opts.timeout_ms = 500;
  opts.max_retry = 0;
  KVClient client(opts);

  // Put 应该失败（连接超时）
  bool ok = client.Put("key", "val");
  EXPECT_FALSE(ok);

  // Get 应该失败
  std::string val;
  bool found = false;
  ok = client.Get("key", &val, &found);
  EXPECT_FALSE(ok);
}

// ── 以下测试需要集群运行，标记为 DISABLED（手动启用）────────────────

TEST(DISABLED_ClientIntegrationTest, PutGetDeleteScan) {
  ClientOptions opts;
  opts.peers = "127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202";
  opts.linearizable = true;
  KVClient client(opts);

  // Put
  ASSERT_TRUE(client.Put("test_k1", "test_v1"));

  // Get（线性一致读）
  std::string value;
  bool found = false;
  ASSERT_TRUE(client.Get("test_k1", &value, &found));
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "test_v1");

  // Delete
  ASSERT_TRUE(client.Delete("test_k1"));

  // Get after Delete
  ASSERT_TRUE(client.Get("test_k1", &value, &found));
  EXPECT_FALSE(found);
}

TEST(DISABLED_ClientIntegrationTest, ScanAfterPuts) {
  ClientOptions opts;
  opts.peers = "127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202";
  KVClient client(opts);

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(client.Put("scan_key_" + std::to_string(i),
                           "val_" + std::to_string(i)));
  }

  std::vector<std::pair<std::string, std::string>> kvs;
  ASSERT_TRUE(client.Scan("scan_key_", "scan_key_z", 100, &kvs));
  EXPECT_GE(kvs.size(), 5u);
}

TEST(DISABLED_ClientIntegrationTest, RedirectToLeader) {
  // 故意连接 Follower 节点，验证自动 redirect
  ClientOptions opts;
  opts.peers = "127.0.0.1:8201"; // 假设 8201 是 Follower
  opts.linearizable = false;     // 弱一致读不 redirect
  KVClient client(opts);

  // Put 必须走 Leader，应自动 redirect
  ASSERT_TRUE(client.Put("redirect_test", "ok"));

  std::string value;
  bool found = false;
  ASSERT_TRUE(client.Get("redirect_test", &value, &found));
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "ok");
}

} // namespace raftkv