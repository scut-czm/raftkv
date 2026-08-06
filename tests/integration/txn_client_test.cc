// tests/integration/txn_client_test.cc
// 客户端事务库（Transaction + KVClient 事务接口）集成测试。
// 前置：fixture 自行 fork 一个单节点 kv_server，走真实 RPC。
//
// 覆盖三个场景：
//   1. 两事务并发写同 key：一个成功、一个写冲突，重跑后成功；
//   2. 事务 A prewrite 后假死（不 commit）：B 读 → 等 TTL 过期 →
//      resolve 残锁 → 读到旧值；
//   3. primary 已 commit、secondary 未 commit：读 secondary →
//      帮提交（用 primary 的 commit_ts 推进）→ 读到新值。

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "client/kv_client.h"
#include "client/transaction.h"

namespace {

constexpr int kPort = 8310;
constexpr char kServerAddr[] = "127.0.0.1:8310";

class TxnClientTest : public ::testing::Test {
protected:
  static pid_t server_pid_;

  static void SetUpTestSuite() {
    std::string data_dir = "/tmp/raftkv_txn_client_" + std::to_string(kPort);
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    server_pid_ = fork();
    ASSERT_NE(server_pid_, -1);
    if (server_pid_ == 0) {
      execl("./kv_server", "kv_server", "--port=8310", "--ip=127.0.0.1",
            "--group=TxnClientGroup", "--conf=127.0.0.1:8310:0",
            ("--data_path=" + data_dir).c_str(),
            "--raft_enable_leader_lease=true", "--election_timeout_ms=500",
            (char *)nullptr);
      _exit(127);
    }
    // 等 leader 就绪：GetTso 成功即代表选主完成
    // 花括号初始化，避免 most vexing parse（否则被解析成函数声明）
    raftkv::KVClient probe{std::string(kServerAddr)};
    for (int i = 0; i < 50; ++i) {
      if (probe.GetTso(1) != 0) {
        return;
      }
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
    client_ = std::make_unique<raftkv::KVClient>(std::string(kServerAddr));
  }

  // 用一个独立事务写入并提交一对 kv（准备初始数据用）
  void CommitValue(const std::string &key, const std::string &value) {
    raftkv::Transaction txn(client_.get());
    txn.Put(key, value);
    std::string err;
    ASSERT_TRUE(txn.Commit(&err)) << err;
  }

  std::unique_ptr<raftkv::KVClient> client_;
};
pid_t TxnClientTest::server_pid_ = 0;

// ---- 场景 1：并发写同 key，一个成功一个冲突，重跑后成功 ----
TEST_F(TxnClientTest, ConcurrentWriteConflictThenRetrySucceeds) {
  CommitValue("cc_k", "v0");

  // 两个事务同时开启（快照点都在对方提交之前），写同一个 key
  raftkv::Transaction t1(client_.get());
  raftkv::Transaction t2(client_.get());
  ASSERT_GT(t1.start_ts(), 0u);
  ASSERT_GT(t2.start_ts(), t1.start_ts());
  t1.Put("cc_k", "from_t1");
  t2.Put("cc_k", "from_t2");

  // t2 先提交成功
  std::string err2;
  ASSERT_TRUE(t2.Commit(&err2)) << err2;
  ASSERT_GT(t2.commit_ts(), t2.start_ts());

  // t1 提交必须失败：它的 start_ts 早于 cc_k 上已提交的 commit_ts（写冲突）
  std::string err1;
  EXPECT_FALSE(t1.Commit(&err1));
  EXPECT_FALSE(err1.empty());

  // 乐观模型的正确姿势：整个事务重跑（新 start_ts）→ 成功
  raftkv::Transaction retry(client_.get());
  auto cur = retry.Get("cc_k");
  ASSERT_TRUE(cur.has_value());
  EXPECT_EQ(*cur, "from_t2"); // 重跑事务看到 t2 的提交
  retry.Put("cc_k", "from_t1_retry");
  std::string err3;
  ASSERT_TRUE(retry.Commit(&err3)) << err3;

  raftkv::Transaction reader(client_.get());
  auto final_val = reader.Get("cc_k");
  ASSERT_TRUE(final_val.has_value());
  EXPECT_EQ(*final_val, "from_t1_retry");
}

// ---- 场景 2：prewrite 后假死，读者等 TTL 过期 → resolve → 读旧值 ----
TEST_F(TxnClientTest, ReaderResolvesExpiredLockAndReadsOldValue) {
  CommitValue("ttl_k", "old");

  // 事务 A：绕过 Transaction 直接 prewrite（短 TTL），随后"客户端假死"不提交
  const uint64_t a_start = client_->GetTso(1);
  ASSERT_GT(a_start, 0u);
  std::vector<raftkv::TxnMutation> muts;
  raftkv::TxnMutation m;
  m.key = "ttl_k";
  m.value = "uncommitted";
  muts.push_back(std::move(m));
  raftkv::LockInfo conflict;
  std::string perr;
  ASSERT_TRUE(client_->TxnPrewrite(muts, /*primary=*/"ttl_k", a_start,
                                   /*ttl_ms=*/200, &conflict, &perr))
      << perr;
  // ……A 假死，锁留在 ttl_k 上……

  // 事务 B：Get 内部撞锁 → CheckTxnStatus 判定存活 → backoff →
  // TTL 过期后再 Check → 服务端回滚 A → B 读到旧值
  raftkv::Transaction b(client_.get());
  auto val = b.Get("ttl_k");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "old"); // A 未提交的 "uncommitted" 绝不能可见

  // A 的迟到 Commit 必须失败（回滚墓碑已写）
  uint64_t late_cts = 0;
  std::string cerr;
  EXPECT_FALSE(
      client_->TxnCommit({"ttl_k"}, a_start, 0, &late_cts, &cerr));
}

// ---- 场景 3：primary 已提交、secondary 残锁 → 读者帮提交读到新值 ----
TEST_F(TxnClientTest, ReaderHelpsCommitSecondaryAndReadsNewValue) {
  CommitValue("sec_k", "sec_old");

  // 一个事务写 primary=pri_k 和 secondary=sec_k 两把锁
  const uint64_t start = client_->GetTso(1);
  ASSERT_GT(start, 0u);
  std::vector<raftkv::TxnMutation> muts;
  raftkv::TxnMutation m1;
  m1.key = "pri_k";
  m1.value = "pri_new";
  raftkv::TxnMutation m2;
  m2.key = "sec_k";
  m2.value = "sec_new";
  muts.push_back(std::move(m1));
  muts.push_back(std::move(m2));
  raftkv::LockInfo conflict;
  std::string perr;
  ASSERT_TRUE(client_->TxnPrewrite(muts, /*primary=*/"pri_k", start,
                                   /*ttl_ms=*/60000, &conflict, &perr))
      << perr;

  // 只提交 primary（模拟客户端提交到一半崩溃），secondary 残锁
  uint64_t commit_ts = 0;
  std::string cerr;
  ASSERT_TRUE(client_->TxnCommit({"pri_k"}, start, 0, &commit_ts, &cerr))
      << cerr;
  ASSERT_GT(commit_ts, start);

  // 读者读 secondary：撞锁 → CheckTxnStatus(primary)=已提交 →
  // 用同一 commit_ts 帮提交 sec_k → 读到新值。
  // 注意 TTL 是 60s：若没有"帮提交"，这里只能 backoff 到重试耗尽读失败。
  raftkv::Transaction reader(client_.get());
  auto val = reader.Get("sec_k");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "sec_new");

  // 帮提交必须复用 primary 的 commit_ts：两 key 的版本完全一致
  raftkv::Transaction verify(client_.get());
  auto pv = verify.Get("pri_k");
  ASSERT_TRUE(pv.has_value());
  EXPECT_EQ(*pv, "pri_new");
}

} // namespace
