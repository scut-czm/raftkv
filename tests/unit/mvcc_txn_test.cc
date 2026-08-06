// MVCC 事务层单元测试（GoogleTest）。
// 直接构造 MvccTxn 在本地 RocksDB 上测试，不经过 Raft —— 因为写路径在
// on_apply 中也是同样的串行调用，单测覆盖的正是这段逻辑。

#include <filesystem>
#include <gtest/gtest.h>

#include "storage/mvcc_codec.h"
#include "storage/mvcc_txn.h"
#include "storage/rocksdb_storage.h"

namespace raftkv {
namespace {

class MvccTxnTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string path = ::testing::TempDir() + "/mvcc_test_" + info->name();
    std::filesystem::remove_all(path); // 清掉上次运行的残留
    storage_ = std::make_unique<RocksDbStorage>(StorageOptions{path});
    ASSERT_TRUE(storage_->Open());
    txn_ = std::make_unique<MvccTxn>(storage_.get());
  }

  // ts 语义化小工具：让用例里的时间戳有可读性。
  static constexpr uint64_t T(uint64_t n) { return n << 18; }

  std::unique_ptr<RocksDbStorage> storage_;
  std::unique_ptr<MvccTxn> txn_;
};

// ---- 编码性质 ----
TEST(MvccCodecTest, NewerVersionSortsFirst) {
  // 同 key 下 ts 大的编码后字典序小（Seek 先命中新版本）。
  EXPECT_LT(MvccCodec::EncodeKey("k", 20), MvccCodec::EncodeKey("k", 10));
  // 不同 key 仍按 key 排序。
  EXPECT_LT(MvccCodec::EncodeKey("a", 1), MvccCodec::EncodeKey("b", 999));

  std::string_view uk;
  uint64_t ts = 0;
  ASSERT_TRUE(MvccCodec::DecodeKey(MvccCodec::EncodeKey("key", 42), &uk, &ts));
  EXPECT_EQ(uk, "key");
  EXPECT_EQ(ts, 42u);
}

// ---- 快照可见性 ----
TEST_F(MvccTxnTest, SnapshotVisibility) {
  // T10 开始的事务在 T20 提交 k=v1。
  ASSERT_TRUE(txn_->Prewrite("k", "v1", false, "k", T(10), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(10), T(20)).ok());

  std::optional<std::string> v;
  // T15 的快照：看不到 T20 的提交。
  ASSERT_TRUE(txn_->Get("k", T(15), &v).ok());
  EXPECT_FALSE(v.has_value());
  // T25 的快照：可见。
  ASSERT_TRUE(txn_->Get("k", T(25), &v).ok());
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "v1");
}

TEST_F(MvccTxnTest, MultipleVersionsPickLatestVisible) {
  ASSERT_TRUE(txn_->Prewrite("k", "v1", false, "k", T(10), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(10), T(20)).ok());
  ASSERT_TRUE(txn_->Prewrite("k", "v2", false, "k", T(30), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(30), T(40)).ok());

  std::optional<std::string> v;
  ASSERT_TRUE(txn_->Get("k", T(25), &v).ok());
  EXPECT_EQ(*v, "v1"); // 旧快照读旧版本
  ASSERT_TRUE(txn_->Get("k", T(50), &v).ok());
  EXPECT_EQ(*v, "v2"); // 新快照读新版本
}

// ---- 写冲突 ----
TEST_F(MvccTxnTest, WriteConflict) {
  // txn A (start=T10) 与 txn B (start=T15) 都要写 k；B 先提交。
  ASSERT_TRUE(txn_->Prewrite("k", "b", false, "k", T(15), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(15), T(20)).ok());
  // A 再 Prewrite：commit T20 >= start T10 → 冲突。
  MvccError err = txn_->Prewrite("k", "a", false, "k", T(10), 3000);
  EXPECT_EQ(err.kind, MvccError::kWriteConflict);
  EXPECT_EQ(err.conflict_commit_ts, T(20));
}

// ---- 锁阻塞读 ----
TEST_F(MvccTxnTest, LockBlocksReader) {
  ASSERT_TRUE(txn_->Prewrite("k", "v", false, "k", T(10), 3000).ok());
  std::optional<std::string> v;
  // 快照 >= lock start_ts：被阻塞。
  MvccError err = txn_->Get("k", T(15), &v);
  EXPECT_EQ(err.kind, MvccError::kLocked);
  EXPECT_EQ(err.lock_ts, T(10));
  // 快照 < lock start_ts：不受影响（该事务即便提交 commit_ts 也 > 快照）。
  EXPECT_TRUE(txn_->Get("k", T(5), &v).ok());
}

// ---- 幂等 ----
TEST_F(MvccTxnTest, IdempotentPrewriteAndCommit) {
  ASSERT_TRUE(txn_->Prewrite("k", "v", false, "k", T(10), 3000).ok());
  ASSERT_TRUE(txn_->Prewrite("k", "v", false, "k", T(10), 3000).ok()); // 重试
  ASSERT_TRUE(txn_->Commit("k", T(10), T(20)).ok());
  ASSERT_TRUE(txn_->Commit("k", T(10), T(20)).ok()); // 重试
}

// ---- 回滚墓碑挡住迟到的 Prewrite ----
TEST_F(MvccTxnTest, RollbackTombstoneBlocksLatePrewrite) {
  ASSERT_TRUE(txn_->Rollback("k", T(10)).ok()); // 先回滚（如 TTL 超时被判死）
  MvccError err = txn_->Prewrite("k", "v", false, "k", T(10), 3000);
  EXPECT_EQ(err.kind, MvccError::kAlreadyRolledBack);
  // 且已提交的事务不能被回滚。
  ASSERT_TRUE(txn_->Prewrite("k2", "v", false, "k2", T(30), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k2", T(30), T(40)).ok());
  EXPECT_EQ(txn_->Rollback("k2", T(30)).kind, MvccError::kTxnNotFound);
}

// ---- CheckTxnStatus 四分支 ----
TEST_F(MvccTxnTest, CheckTxnStatusBranches) {
  TxnStatus st;
  // 分支1：已提交。
  ASSERT_TRUE(txn_->Prewrite("p1", "v", false, "p1", T(10), 3000).ok());
  ASSERT_TRUE(txn_->Commit("p1", T(10), T(20)).ok());
  ASSERT_TRUE(txn_->CheckTxnStatus("p1", T(10), T(99), &st).ok());
  EXPECT_EQ(st.state, TxnStatus::kCommitted);
  EXPECT_EQ(st.commit_ts, T(20));

  // 分支3：锁存活（TTL 未过）。
  ASSERT_TRUE(txn_->Prewrite("p2", "v", false, "p2", T(30), 10000).ok());
  ASSERT_TRUE(txn_->CheckTxnStatus("p2", T(30), T(31), &st).ok());
  EXPECT_EQ(st.state, TxnStatus::kLockAlive);
  EXPECT_GT(st.remaining_ttl_ms, 0u);

  // 分支4：锁超 TTL → 被判死（回滚）。
  ASSERT_TRUE(
      txn_->CheckTxnStatus("p2", T(30), T(30) + (20000ull << 18), &st).ok());
  EXPECT_EQ(st.state, TxnStatus::kRolledBack);

  // 防御分支：锁不存在且无记录 → 写墓碑判死。
  ASSERT_TRUE(txn_->CheckTxnStatus("ghost", T(50), T(60), &st).ok());
  EXPECT_EQ(st.state, TxnStatus::kRolledBack);
  EXPECT_EQ(txn_->Prewrite("ghost", "v", false, "ghost", T(50), 3000).kind,
            MvccError::kAlreadyRolledBack);
}

// ---- DELETE 的可见性 ----
TEST_F(MvccTxnTest, DeleteVisibility) {
  ASSERT_TRUE(txn_->Prewrite("k", "v", false, "k", T(10), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(10), T(20)).ok());
  ASSERT_TRUE(txn_->Prewrite("k", "", true, "k", T(30), 3000).ok());
  ASSERT_TRUE(txn_->Commit("k", T(30), T(40)).ok());

  std::optional<std::string> v;
  ASSERT_TRUE(txn_->Get("k", T(25), &v).ok());
  EXPECT_TRUE(v.has_value()); // 删除前的快照仍可见
  ASSERT_TRUE(txn_->Get("k", T(50), &v).ok());
  EXPECT_FALSE(v.has_value()); // 删除后不可见
}

// ---- Scan 快照语义 ----
TEST_F(MvccTxnTest, ScanSnapshot) {
  for (int i = 0; i < 5; ++i) {
    std::string k = "k" + std::to_string(i);
    ASSERT_TRUE(
        txn_->Prewrite(k, "v" + std::to_string(i), false, k, T(10 + i), 3000)
            .ok());
    ASSERT_TRUE(txn_->Commit(k, T(10 + i), T(20 + i)).ok());
  }
  std::vector<std::pair<std::string, std::string>> rows;
  // 快照 T22：只有 commit_ts <= T22 的 k0、k1、k2 可见。
  ASSERT_TRUE(txn_->Scan("k0", "k9", 100, T(22), &rows).ok());
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].first, "k0");
  EXPECT_EQ(rows[2].second, "v2");
}

} // namespace
} // namespace raftkv