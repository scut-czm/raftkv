#include "raft/kv_state_machine.h"
#include "storage/rocksdb_storage.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace fs = std::filesystem;

namespace raftkv {
class StateMachineTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ = "/tmp/raftkv_sm_test_" + std::to_string(getpid());
    fs::remove_all(db_path_);
    storage_ = std::make_shared<RocksDbStorage>(StorageOptions{db_path_});
    ASSERT_TRUE(storage_->Open());
    fsm_ = std::make_unique<KVStateMachine>(storage_);
  }

  void TearDown() override {
    fsm_.reset();
    storage_->Close();
    fs::remove_all(db_path_);
  }

  std::string db_path_;
  std::shared_ptr<RocksDbStorage> storage_;
  std::unique_ptr<KVStateMachine> fsm_;
};

// ── Get / Scan 接口（通过 storage 直接写入，验证 FSM 读取）─────────────
TEST_F(StateMachineTest, GetAfterStoragePut) {
  ASSERT_TRUE(storage_->Put("hello", "world"));
  std::string value;
  EXPECT_TRUE(fsm_->Get("hello", &value));
  EXPECT_EQ(value, "world");
}
TEST_F(StateMachineTest, GetNotFound) {
  std::string value;
  EXPECT_FALSE(fsm_->Get("not_exist", &value));
}

TEST_F(StateMachineTest, ScanAfterStoragePuts) {
  ASSERT_TRUE(storage_->Put("a", "1"));
  ASSERT_TRUE(storage_->Put("b", "2"));
  ASSERT_TRUE(storage_->Put("c", "3"));

  auto kvs = fsm_->Scan("a", "d", 0);
  ASSERT_EQ(kvs.size(), 3u);
  EXPECT_EQ(kvs[0].first, "a");
  EXPECT_EQ(kvs[2].first, "c");
}
TEST_F(StateMachineTest, ScanWithLimit) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(storage_->Put("key" + std::to_string(i), "v"));
  }
  auto kvs = fsm_->Scan("key0", "", 2);
  EXPECT_EQ(kvs.size(), 2u);
}
// ── 快照（Checkpoint）保存与恢复 ─────────────────────────────────────

TEST_F(StateMachineTest, CheckpointSaveAndRestore) {
  // 写入初始数据
  ASSERT_TRUE(storage_->Put("k1", "v1"));
  ASSERT_TRUE(storage_->Put("k2", "v2"));

  // 保存 Checkpoint
  std::string ck_path = "/tmp/raftkv_sm_ck_" + std::to_string(getpid());
  fs::remove_all(ck_path);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck_path));

  // 写入新数据（Checkpoint 之后）
  ASSERT_TRUE(storage_->Put("k3", "after_ck"));

  // 恢复到 Checkpoint
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck_path));

  // 验证：k1/k2 存在，k3 不存在
  std::string value;
  EXPECT_TRUE(fsm_->Get("k1", &value));
  EXPECT_EQ(value, "v1");
  EXPECT_TRUE(fsm_->Get("k2", &value));
  EXPECT_EQ(value, "v2");
  EXPECT_FALSE(fsm_->Get("k3", &value));

  fs::remove_all(ck_path);
}
TEST_F(StateMachineTest, CheckpointRestoreEmptyDb) {
  // 空数据库保存 Checkpoint，写入数据后恢复，应清空
  std::string ck_path = "/tmp/raftkv_sm_ck_empty_" + std::to_string(getpid());
  fs::remove_all(ck_path);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck_path));

  ASSERT_TRUE(storage_->Put("should_vanish", "val"));
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck_path));

  std::string value;
  EXPECT_FALSE(fsm_->Get("should_vanish", &value));

  fs::remove_all(ck_path);
}

// ── IsLeader 初始状态 ─────────────────────────────────────────────────

TEST_F(StateMachineTest, InitiallyNotLeader) { EXPECT_FALSE(fsm_->IsLeader()); }

// ── 大量数据快照恢复 ──────────────────────────────────────────────────
TEST_F(StateMachineTest, CheckpointWithLargeData) {
  // 写入 1000 条数据
  for (int i = 0; i < 1000; ++i) {
    std::string key = "bulk_" + std::to_string(i);
    std::string val = "value_" + std::to_string(i);
    ASSERT_TRUE(storage_->Put(key, val));
  }
  // 快照
  std::string ck_path = "/tmp/raftkv_sm_ck_bulk_" + std::to_string(getpid());
  fs::remove_all(ck_path);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck_path));

  // 写入额外数据
  ASSERT_TRUE(storage_->Put("after_bulk", "should_vanish"));

  // 恢复
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck_path));

  // 验证：1000 条原始数据完整，额外数据消失
  for (int i = 0; i < 1000; ++i) {
    std::string key = "bulk_" + std::to_string(i);
    std::string val;
    ASSERT_TRUE(fsm_->Get(key, &val)) << "missing: " << key;
    EXPECT_EQ(val, "value_" + std::to_string(i));
  }
  std::string val;
  EXPECT_FALSE(fsm_->Get("after_bulk", &val));

  fs::remove_all(ck_path);
}
// ── 多次快照恢复 ──────────────────────────────────────────────────────
TEST_F(StateMachineTest, MultipleCheckpointRestore) {
  // 第一次快照：写入 k1
  ASSERT_TRUE(storage_->Put("k1", "v1"));
  std::string ck1 = "/tmp/raftkv_sm_ck1_" + std::to_string(getpid());
  fs::remove_all(ck1);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck1));

  // 第二次快照：写入 k2
  ASSERT_TRUE(storage_->Put("k2", "v2"));
  std::string ck2 = "/tmp/raftkv_sm_ck2_" + std::to_string(getpid());
  fs::remove_all(ck2);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck2));

  // 写入 k3（不在任何快照中）
  ASSERT_TRUE(storage_->Put("k3", "v3"));

  // 恢复到 ck1：只有 k1
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck1));
  std::string val;
  EXPECT_TRUE(fsm_->Get("k1", &val));
  EXPECT_FALSE(fsm_->Get("k2", &val));
  EXPECT_FALSE(fsm_->Get("k3", &val));

  // 恢复到 ck2：有 k1 和 k2
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck2));
  EXPECT_TRUE(fsm_->Get("k1", &val));
  EXPECT_TRUE(fsm_->Get("k2", &val));
  EXPECT_FALSE(fsm_->Get("k3", &val));

  fs::remove_all(ck1);
  fs::remove_all(ck2);
}
// ── Scan 边界 ─────────────────────────────────────────────────────────

TEST_F(StateMachineTest, ScanEmptyDatabase) {
  auto kvs = fsm_->Scan("a", "z", 0);
  EXPECT_TRUE(kvs.empty());
}

TEST_F(StateMachineTest, ScanSingleKey) {
  ASSERT_TRUE(storage_->Put("only", "one"));
  auto kvs = fsm_->Scan("only", "onlz", 0);
  ASSERT_EQ(kvs.size(), 1u);
  EXPECT_EQ(kvs[0].first, "only");
  EXPECT_EQ(kvs[0].second, "one");
}

// ── Get 写入后立即读 ──────────────────────────────────────────────────

TEST_F(StateMachineTest, GetAfterOverwrite) {
  ASSERT_TRUE(storage_->Put("ow_key", "v1"));
  ASSERT_TRUE(storage_->Put("ow_key", "v2"));
  std::string val;
  EXPECT_TRUE(fsm_->Get("ow_key", &val));
  EXPECT_EQ(val, "v2");
}

TEST_F(StateMachineTest, GetAfterDelete) {
  ASSERT_TRUE(storage_->Put("del_key", "val"));
  ASSERT_TRUE(storage_->Delete("del_key"));
  std::string val;
  EXPECT_FALSE(fsm_->Get("del_key", &val));
}

} // namespace raftkv