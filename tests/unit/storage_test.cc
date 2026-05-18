#include "storage/rocksdb_storage.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace raftkv {

class StorageTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ = "/tmp/raftkv_storage_test_" + std::to_string(getpid());
    storage_ = std::make_unique<RocksDbStorage>(StorageOptions{db_path_});
    ASSERT_TRUE(storage_->Open());
  }

  void TearDown() override {
    storage_->Close();
    fs::remove_all(db_path_);
  }
  std::string db_path_;
  std::unique_ptr<RocksDbStorage> storage_;
};

// ── Put / Get / Delete ──────────────────────────────────────────────────

TEST_F(StorageTest, PutAndGet) {
  ASSERT_TRUE(storage_->Put("hello", "world"));
  std::string value;
  ASSERT_TRUE(storage_->Get("hello", &value));
  EXPECT_EQ(value, "world");
}

TEST_F(StorageTest, GetNotFound) {
  std::string value;
  EXPECT_FALSE(storage_->Get("nonexistent", &value));
}

TEST_F(StorageTest, DeleteKey) {
  ASSERT_TRUE(storage_->Put("k1", "v1"));
  ASSERT_TRUE(storage_->Delete("k1"));
  std::string value;
  EXPECT_FALSE(storage_->Get("k1", &value));
}
TEST_F(StorageTest, OverwriteValue) {
  ASSERT_TRUE(storage_->Put("key", "old"));
  ASSERT_TRUE(storage_->Put("key", "new"));
  std::string value;
  ASSERT_TRUE(storage_->Get("key", &value));
  EXPECT_EQ(value, "new");
}

// ── Scan ─────────────────────────────────────────────────────────────────

TEST_F(StorageTest, ScanOrder) {
  // 写入乱序，期望 Scan 按字典序返回
  ASSERT_TRUE(storage_->Put("c", "3"));
  ASSERT_TRUE(storage_->Put("a", "1"));
  ASSERT_TRUE(storage_->Put("b", "2"));

  auto kvs = storage_->Scan("a", "d", 0);
  ASSERT_EQ(kvs.size(), 3u);
  EXPECT_EQ(kvs[0].first, "a");
  EXPECT_EQ(kvs[1].first, "b");
  EXPECT_EQ(kvs[2].first, "c");
}

TEST_F(StorageTest, ScanWithLimit) {
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(storage_->Put("key" + std::to_string(i), "val"));
  }
  auto kvs = storage_->Scan("key0", "", 3);
  EXPECT_EQ(kvs.size(), 3u);
}

TEST_F(StorageTest, ScanEndKeyExclusive) {
  ASSERT_TRUE(storage_->Put("a", "1"));
  ASSERT_TRUE(storage_->Put("b", "2"));
  ASSERT_TRUE(storage_->Put("c", "3"));

  // end_key="c"，不含 c
  auto kvs = storage_->Scan("a", "c", 0);
  ASSERT_EQ(kvs.size(), 2u);
  EXPECT_EQ(kvs.back().first, "b");
}
TEST_F(StorageTest, ScanEmptyRange) {
  auto kvs = storage_->Scan("x", "z", 0);
  EXPECT_TRUE(kvs.empty());
}

// ── BatchWrite ────────────────────────────────────────────────────────────
TEST_F(StorageTest, BatchWrite) {
  std::vector<std::pair<std::string, std::string>> puts = {
      {"b1", "v1"}, {"b2", "v2"}, {"b3", "v3"}};
  ASSERT_TRUE(storage_->BatchWrite(puts, {}));
  std::string value;
  ASSERT_TRUE(storage_->Get("b2", &value));
  EXPECT_EQ(value, "v2");
}
TEST_F(StorageTest, BatchWriteWithDeletes) {
  ASSERT_TRUE(storage_->Put("del_me", "val"));
  std::vector<std::pair<std::string, std::string>> puts = {{"new_key", "nv"}};
  ASSERT_TRUE(storage_->BatchWrite(puts, {"del_me"}));

  std::string value;
  EXPECT_FALSE(storage_->Get("del_me", &value));
  ASSERT_TRUE(storage_->Get("new_key", &value));
  EXPECT_EQ(value, "nv");
}
// ── meta CF ──────────────────────────────────────────────────────────────
TEST_F(StorageTest, MetaCFIsolated) {
  ASSERT_TRUE(storage_->Put("key", "data_val"));
  ASSERT_TRUE(storage_->PutMeta("key", "meta_val"));

  std::string data_val, meta_val;
  ASSERT_TRUE(storage_->Get("key", &data_val));
  ASSERT_TRUE(storage_->GetMeta("key", &meta_val));

  // data CF 和 meta CF 相互隔离
  EXPECT_EQ(data_val, "data_val");
  EXPECT_EQ(meta_val, "meta_val");
}
// ── Checkpoint ────────────────────────────────────────────────────────────

TEST_F(StorageTest, CreateAndRestoreCheckpoint) {
  ASSERT_TRUE(storage_->Put("ck_key", "ck_val"));

  std::string ck_path = "/tmp/raftkv_ck_test_" + std::to_string(getpid());
  fs::remove_all(ck_path);

  ASSERT_TRUE(storage_->CreateCheckpoint(ck_path));

  // 写入新数据后恢复快照，新数据应消失
  ASSERT_TRUE(storage_->Put("after_ck", "should_vanish"));
  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck_path));

  std::string value;
  ASSERT_TRUE(storage_->Get("ck_key", &value));
  EXPECT_EQ(value, "ck_val");
  EXPECT_FALSE(storage_->Get("after_ck", &value));

  fs::remove_all(ck_path);
}

// ── 并发安全（RocksDB 内部线程安全验证）─────────────────────────────
TEST_F(StorageTest, ConcurrentPutAndGet) {
  const int kThreads = 8;
  const int kOpsPerThread = 500;
  std::vector<std::thread> writers;
  std::atomic<int> errors{0};
  // 多线程并发写入
  for (int t = 0; t < kThreads; t++) {
    writers.emplace_back([&, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
        std::string val = "v" + std::to_string(i);
        if (!storage_->Put(key, val)) {
          errors.fetch_add(1);
        }
      }
    });
  }
  for (auto &w : writers) {
    w.join();
  }
  EXPECT_EQ(errors.load(), 0);

  // 验证所有写入的数据均可读取
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
      std::string val;
      ASSERT_TRUE(storage_->Get(key, &val)) << "missing key: " << key;
      EXPECT_EQ(val, "v" + std::to_string(i));
    }
  }
}

TEST_F(StorageTest, ConcurrentReadWrite) {
  // 先写入一批数据
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(
        storage_->Put("rw_" + std::to_string(i), "val_" + std::to_string(i)));
  }

  std::atomic<bool> running{true};
  std::atomic<int> read_errors{0};
  std::atomic<int> write_errors{0};

  // 写线程：持续覆盖写
  std::thread writer([&]() {
    int seq = 0;
    while (running.load()) {
      std::string key = "rw_" + std::to_string(seq % 100);
      if (!storage_->Put(key, "updated_" + std::to_string(seq))) {
        write_errors.fetch_add(1);
      }
      ++seq;
    }
  });

  // 读线程：持续读取
  std::thread reader([&]() {
    while (running.load()) {
      for (int i = 0; i < 100; ++i) {
        std::string key = "rw_" + std::to_string(i);
        std::string val;
        if (!storage_->Get(key, &val)) {
          read_errors.fetch_add(1);
        }
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  running.store(false);
  writer.join();
  reader.join();

  EXPECT_EQ(read_errors.load(), 0);
  EXPECT_EQ(write_errors.load(), 0);
}

//============================================================
// ── 边界情况 ──────────────────────────────────────────────────────────

TEST_F(StorageTest, EmptyKeyAndValue) {
  ASSERT_TRUE(storage_->Put("", "empty_key_val"));
  std::string val;
  ASSERT_TRUE(storage_->Get("", &val));
  EXPECT_EQ(val, "empty_key_val");

  ASSERT_TRUE(storage_->Put("empty_val", ""));
  ASSERT_TRUE(storage_->Get("empty_val", &val));
  EXPECT_EQ(val, "");
}

TEST_F(StorageTest, LargeValue) {
  // 1MB value
  std::string large_val(1024 * 1024, 'x');
  ASSERT_TRUE(storage_->Put("big", large_val));
  std::string val;
  ASSERT_TRUE(storage_->Get("big", &val));
  EXPECT_EQ(val.size(), large_val.size());
  EXPECT_EQ(val, large_val);
}
TEST_F(StorageTest, DeleteNonExistent) {
  // 删除不存在的 key 应成功（RocksDB 不报错）
  EXPECT_TRUE(storage_->Delete("never_existed"));
}

TEST_F(StorageTest, DeleteTwice) {
  ASSERT_TRUE(storage_->Put("dup_del", "val"));
  ASSERT_TRUE(storage_->Delete("dup_del"));
  // 第二次删除也应成功
  EXPECT_TRUE(storage_->Delete("dup_del"));
}

TEST_F(StorageTest, ScanFullRange) {
  // end_key 为空 = 扫描到末尾
  ASSERT_TRUE(storage_->Put("a", "1"));
  ASSERT_TRUE(storage_->Put("m", "2"));
  ASSERT_TRUE(storage_->Put("z", "3"));

  auto kvs = storage_->Scan("", "", 0);
  ASSERT_GE(kvs.size(), 3u);
  // 验证包含所有 key
  bool found_a = false, found_z = false;
  for (const auto &[k, v] : kvs) {
    if (k == "a")
      found_a = true;
    if (k == "z")
      found_z = true;
  }
  EXPECT_TRUE(found_a);
  EXPECT_TRUE(found_z);
}

TEST_F(StorageTest, BatchWriteEmpty) {
  // 空批量写入应成功
  EXPECT_TRUE(storage_->BatchWrite({}, {}));
}
} // namespace raftkv
