// tests/integration/txn_integration_test.cc
// 单节点事务链路集成测试：真实 RPC 走 Prewrite → Commit → 快照 Get/Scan。
// 前置：测试 fixture 自行 fork 一个单节点 kv_server。

#include "raft/kv_state_machine.h"
#include <brpc/channel.h>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kv.pb.h"
#include <braft/raft.h>
#include <butil/logging.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

namespace {

constexpr int kPort = 8300;
constexpr char kServerAddr[] = "127.0.0.1:8300";

// 与 kv_service.cc 的 HybridNowTs 相同布局：高 46 位物理毫秒 << 18
uint64_t Ts(uint64_t logical) { return logical << 18; }

// 真实时间版时间戳：CheckTxnStatus 的 TTL 判定用 leader 的 HybridNowTs
// （真实 epoch 毫秒）做 now_ts，所以参与 TTL 用例的 lock_ts 必须也基于
// 真实时间，否则 elapsed = now - lock_ts 会巨大到任何锁都"过期"。
uint64_t NowTs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  uint64_t ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  return ms << 18;
}

class TxnIntegrationTest : public ::testing::Test {
protected:
  static pid_t server_pid_;

  static void SetUpTestSuite() {
    std::string data_dir = "/tmp/raftkv_txn_it_" + std::to_string(kPort);
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    server_pid_ = fork();
    ASSERT_NE(server_pid_, -1);
    if (server_pid_ == 0) {
      // 单节点 conf：只有自己，选举后立即成为 leader
      execl("./kv_server", "kv_server", "--port=8300", "--ip=127.0.0.1",
            "--group=TxnITGroup", "--conf=127.0.0.1:8300:0",
            ("--data_path=" + data_dir).c_str(),
            "--raft_enable_leader_lease=true", "--election_timeout_ms=500",
            (char *)nullptr);
      _exit(127);
    }
    // 等 leader 选出：轮询一个弱读 Get 直到 success
    brpc::Channel ch;
    ASSERT_EQ(ch.Init(kServerAddr, nullptr), 0);
    kv::KvService_Stub stub(&ch);
    for (int i = 0; i < 50; ++i) {
      kv::GetRequest req;
      req.set_key("__probe__");
      kv::GetResponse resp;
      brpc::Controller cntl;
      stub.Get(&cntl, &req, &resp, nullptr);
      if (!cntl.Failed() && resp.success())
        return;
      usleep(200 * 1000);
    }
    FAIL() << "server 未在 10s 内就绪";
  }

  static void TearDownTestSuite() {
    if (server_pid_ > 0) {
      kill(server_pid_, SIGTERM);
      waitpid(server_pid_, nullptr, 0);
    }
  }

  void SetUp() override {
    ASSERT_EQ(channel_.Init(kServerAddr, nullptr), 0);
    stub_ = std::make_unique<kv::KvService_Stub>(&channel_);
  }

  // ---- RPC 小工具 ----
  kv::TxnPrewriteResponse Prewrite(const std::string &key,
                                   const std::string &value,
                                   const std::string &primary,
                                   uint64_t start_ts, bool is_delete = false,
                                   uint64_t ttl_ms = 3000) {
    kv::TxnPrewriteRequest req;
    auto *m = req.add_mutations();
    m->set_op(is_delete ? kv::Mutation::DELETE : kv::Mutation::PUT);
    m->set_key(key);
    m->set_value(value);
    req.set_primary(primary);
    req.set_start_ts(start_ts);
    req.set_ttl_ms(ttl_ms);
    kv::TxnPrewriteResponse resp;
    brpc::Controller cntl;
    stub_->TxnPrewrite(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::TxnCommitResponse Commit(const std::string &key, uint64_t start_ts,
                               uint64_t commit_ts = 0) {
    kv::TxnCommitRequest req;
    req.add_keys(key);
    req.set_start_ts(start_ts);
    req.set_commit_ts(commit_ts);
    kv::TxnCommitResponse resp;
    brpc::Controller cntl;
    stub_->TxnCommit(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::TxnRollbackResponse Rollback(const std::string &key, uint64_t start_ts) {
    kv::TxnRollbackRequest req;
    req.add_keys(key);
    req.set_start_ts(start_ts);
    kv::TxnRollbackResponse resp;
    brpc::Controller cntl;
    stub_->TxnRollback(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::CheckTxnStatusResponse Check(const std::string &primary,
                                   uint64_t lock_ts) {
    kv::CheckTxnStatusRequest req;
    req.set_primary(primary);
    req.set_lock_ts(lock_ts);
    kv::CheckTxnStatusResponse resp;
    brpc::Controller cntl;
    stub_->CheckTxnStatus(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::GetResponse SnapshotGet(const std::string &key, uint64_t snapshot_ts) {
    kv::GetRequest req;
    req.set_key(key);
    req.set_snapshot_ts(snapshot_ts);
    kv::GetResponse resp;
    brpc::Controller cntl;
    stub_->Get(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  brpc::Channel channel_;
  std::unique_ptr<kv::KvService_Stub> stub_;
};
pid_t TxnIntegrationTest::server_pid_ = 0;

// ---- 核心链路：Prewrite → Commit → 快照可见性 ----
TEST_F(TxnIntegrationTest, PrewriteCommitSnapshotGet) {
  const uint64_t start = Ts(100), commit = Ts(200);

  auto pw = Prewrite("k1", "v1", "k1", start);
  ASSERT_TRUE(pw.success()) << pw.error();

  // 提交前，晚于 start 的快照读应报 kLocked（lock_conflict 有值）
  auto locked = SnapshotGet("k1", Ts(150));
  EXPECT_FALSE(locked.success());
  EXPECT_EQ(locked.lock_conflict().key(), "k1");
  EXPECT_EQ(locked.lock_conflict().lock_ts(), start);

  auto cm = Commit("k1", start, commit);
  ASSERT_TRUE(cm.success()) << cm.error();
  EXPECT_EQ(cm.commit_ts(), commit);

  // commit 之后的快照可见，之前的不可见
  auto after = SnapshotGet("k1", Ts(300));
  EXPECT_TRUE(after.success());
  EXPECT_TRUE(after.found());
  EXPECT_EQ(after.value(), "v1");
  auto before = SnapshotGet("k1", Ts(50));
  EXPECT_TRUE(before.success());
  EXPECT_FALSE(before.found());
}
// ---- Rollback：回滚后不可见，迟到 Prewrite 被墓碑挡住 ----
TEST_F(TxnIntegrationTest, RollbackBlocksLatePrewrite) {
  const uint64_t start = Ts(400);
  ASSERT_TRUE(Prewrite("k2", "v2", "k2", start).success());
  ASSERT_TRUE(Rollback("k2", start).success());

  auto g = SnapshotGet("k2", Ts(500));
  EXPECT_TRUE(g.success());
  EXPECT_FALSE(g.found());

  // 同 start_ts 的迟到 Prewrite 必须被拒绝（AlreadyRolledBack）
  auto late = Prewrite("k2", "v2x", "k2", start);
  EXPECT_FALSE(late.success());
}

// ---- 写冲突：后启动事务先提交 → 先启动事务 Prewrite 失败 ----
TEST_F(TxnIntegrationTest, WriteConflict) {
  const uint64_t t1 = Ts(600), t2 = Ts(700);
  ASSERT_TRUE(Prewrite("k3", "new", "k3", t2).success());
  ASSERT_TRUE(Commit("k3", t2, Ts(800)).success());

  auto pw = Prewrite("k3", "old", "k3", t1); // t1 < 已提交的 commit_ts
  EXPECT_FALSE(pw.success());
  EXPECT_EQ(pw.conflict_commit_ts(), Ts(800));
}

// ---- 旧路径回归：snapshot_ts=0 的 Put/Get 不受事务数据影响 ----
TEST_F(TxnIntegrationTest, LegacyPathUnaffected) {
  kv::PutRequest preq;
  preq.set_key("legacy");
  preq.set_value("plain");
  kv::PutResponse presp;
  brpc::Controller c1;
  stub_->Put(&c1, &preq, &presp, nullptr);
  ASSERT_FALSE(c1.Failed());
  ASSERT_TRUE(presp.success());

  auto g = SnapshotGet("legacy", 0); // snapshot_ts=0 → data CF 旧路径
  EXPECT_TRUE(g.success());
  EXPECT_TRUE(g.found());
  EXPECT_EQ(g.value(), "plain");

  // 事务写的 key 不会出现在旧路径读里（不同 CF）
  auto g2 = SnapshotGet("k1", 0);
  EXPECT_FALSE(g2.found());
}

// ---- CheckTxnStatus：锁存活 → remaining_ttl > 0；TTL 过期 → 回滚清锁 ----
TEST_F(TxnIntegrationTest, CheckTxnStatusTtl) {
  // 场景 A：TTL 很长的锁 → kLockAlive，remaining_ttl_ms > 0，锁不动。
  const uint64_t t_alive = NowTs();
  ASSERT_TRUE(Prewrite("c1", "v", "c1", t_alive, false, /*ttl_ms=*/60000)
                  .success());
  auto alive = Check("c1", t_alive);
  ASSERT_TRUE(alive.success()) << alive.error();
  EXPECT_FALSE(alive.committed());
  EXPECT_GT(alive.remaining_ttl_ms(), 0u); // 锁仍存活，调用方应 backoff
  // 锁没被清：快照读仍报 lock_conflict
  auto still = SnapshotGet("c1", NowTs());
  EXPECT_FALSE(still.success());
  EXPECT_EQ(still.lock_conflict().lock_ts(), t_alive);

  // 场景 B：TTL 极短的锁，等它过期 → CheckTxnStatus 当场回滚 primary。
  const uint64_t t_dead = NowTs();
  ASSERT_TRUE(Prewrite("c2", "v", "c2", t_dead, false, /*ttl_ms=*/100)
                  .success());
  usleep(300 * 1000); // 等 TTL 过期
  auto expired = Check("c2", t_dead);
  ASSERT_TRUE(expired.success()) << expired.error();
  EXPECT_FALSE(expired.committed());
  EXPECT_EQ(expired.remaining_ttl_ms(), 0u); // 已回滚
  // 锁已清、数据不可见
  auto g = SnapshotGet("c2", NowTs());
  EXPECT_TRUE(g.success());
  EXPECT_FALSE(g.found());
  // 回滚墓碑已写：同 start_ts 迟到的 Commit 必须失败
  EXPECT_FALSE(Commit("c2", t_dead, NowTs()).success());
}

// ---- CheckTxnStatus：primary 已提交 → 返回 commit_ts，推进 secondary ----
TEST_F(TxnIntegrationTest, CheckTxnStatusCommittedPrimaryResolvesSecondary) {
  const uint64_t start = NowTs();
  // 同一事务两把锁：primary="p1"，secondary="s1"
  ASSERT_TRUE(Prewrite("p1", "pv", "p1", start).success());
  ASSERT_TRUE(Prewrite("s1", "sv", "p1", start).success());

  // 只提交 primary（模拟客户端提交到一半崩溃，secondary 残留锁）
  auto cm = Commit("p1", start, /*commit_ts=*/0); // 0 = leader 分配
  ASSERT_TRUE(cm.success()) << cm.error();
  const uint64_t commit_ts = cm.commit_ts();
  ASSERT_GT(commit_ts, start);

  // 另一个读者被 s1 的残锁挡住 → CheckTxnStatus(primary) 得知已提交
  auto blocked = SnapshotGet("s1", NowTs());
  EXPECT_FALSE(blocked.success());
  EXPECT_EQ(blocked.lock_conflict().primary(), "p1");

  auto st = Check("p1", start);
  ASSERT_TRUE(st.success()) << st.error();
  EXPECT_TRUE(st.committed());
  EXPECT_EQ(st.commit_ts(), commit_ts);

  // 客户端据此用同一 commit_ts 推进 secondary
  ASSERT_TRUE(Commit("s1", start, commit_ts).success());
  auto g = SnapshotGet("s1", NowTs());
  EXPECT_TRUE(g.success());
  EXPECT_TRUE(g.found());
  EXPECT_EQ(g.value(), "sv");
}

} // namespace