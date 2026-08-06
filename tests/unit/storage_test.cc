#include "storage/rocksdb_storage.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

//============================================================
// ── MVCC 三 CF（default / lock / write）──────────────────────────────

// 简易 MVCC key 编码：user_key + 大端存储的 (MAX - ts)，让高 ts 排前面
static std::string EncodeTsKey(const std::string &key, uint64_t ts) {
  uint64_t rev = ~ts;
  std::string out = key;
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((rev >> (i * 8)) & 0xFF));
  }
  return out;
}

TEST_F(StorageTest, MvccGetLockNotFound) {
  // 未写过锁时，lock CF 点查应返回 false
  std::string value;
  EXPECT_FALSE(storage_->Get("lock", "k1", &value));
}

TEST_F(StorageTest, MvccGetUnknownCF) {
  std::string value;
  EXPECT_FALSE(storage_->Get("no_such_cf", "k1", &value));
}

TEST_F(StorageTest, MvccCFIsolation) {
  // 同名 key 写入不同 CF，互不可见
  WriteBatch batch;
  batch.Put("lock", "key", "lock_val");
  batch.Put("default", "key", "default_val");
  batch.Put("write", "key", "write_val");
  ASSERT_TRUE(storage_->Write(std::move(batch)));

  std::string v;
  ASSERT_TRUE(storage_->Get("lock", "key", &v));
  EXPECT_EQ(v, "lock_val");
  ASSERT_TRUE(storage_->Get("default", "key", &v));
  EXPECT_EQ(v, "default_val");
  ASSERT_TRUE(storage_->Get("write", "key", &v));
  EXPECT_EQ(v, "write_val");
  // data CF（旧路径）不受影响
  EXPECT_FALSE(storage_->Get("key", &v));
}

TEST_F(StorageTest, MvccCrossCFAtomicWrite) {
  // 模拟 Prewrite：lock + default 同批写入
  WriteBatch prewrite;
  prewrite.Put("lock", "k1", "lock_info@ts=10");
  prewrite.Put("default", EncodeTsKey("k1", 10), "value_v10");
  ASSERT_TRUE(storage_->Write(std::move(prewrite)));

  std::string v;
  ASSERT_TRUE(storage_->Get("lock", "k1", &v));
  ASSERT_TRUE(storage_->Get("default", EncodeTsKey("k1", 10), &v));
  EXPECT_EQ(v, "value_v10");

  // 模拟 Commit：写 write CF + 删 lock，同批原子生效
  WriteBatch commit;
  commit.Put("write", EncodeTsKey("k1", 12), "write_info(start_ts=10)");
  commit.Delete("lock", "k1");
  ASSERT_TRUE(storage_->Write(std::move(commit)));

  EXPECT_FALSE(storage_->Get("lock", "k1", &v));
  ASSERT_TRUE(storage_->Get("write", EncodeTsKey("k1", 12), &v));
  EXPECT_EQ(v, "write_info(start_ts=10)");
}

TEST_F(StorageTest, MvccWriteUnknownCFRejectsWholeBatch) {
  // 批内含未知 CF：整批拒绝，已知 CF 的操作也不落盘
  WriteBatch batch;
  batch.Put("lock", "k2", "lock_val");
  batch.Put("bad_cf", "k2", "oops");
  EXPECT_FALSE(storage_->Write(std::move(batch)));

  std::string v;
  EXPECT_FALSE(storage_->Get("lock", "k2", &v));
}

TEST_F(StorageTest, MvccWriteCFIterator) {
  // write CF 上按 EncodeTsKey 写入多版本，验证迭代顺序与 Seek 语义
  WriteBatch batch;
  batch.Put("write", EncodeTsKey("k1", 5), "commit@5");
  batch.Put("write", EncodeTsKey("k1", 20), "commit@20");
  batch.Put("write", EncodeTsKey("k2", 8), "commit@8");
  ASSERT_TRUE(storage_->Write(std::move(batch)));

  auto it = storage_->NewIterator("write");
  ASSERT_NE(it, nullptr);

  // Seek(k1@read_ts=15)：应落在 commit_ts <= 15 的最新版本，即 commit@5
  it->Seek(EncodeTsKey("k1", 15));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value().ToString(), "commit@5");

  // Seek(k1@read_ts=100)：应落在最新提交 commit@20
  it->Seek(EncodeTsKey("k1", 100));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value().ToString(), "commit@20");

  // 全量遍历：k1@20, k1@5, k2@8（同 key 内高 ts 在前）
  int count = 0;
  std::vector<std::string> values;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    values.push_back(it->value().ToString());
    ++count;
  }
  ASSERT_EQ(count, 3);
  EXPECT_EQ(values[0], "commit@20");
  EXPECT_EQ(values[1], "commit@5");
  EXPECT_EQ(values[2], "commit@8");

  // 未知 CF 返回 nullptr
  EXPECT_EQ(storage_->NewIterator("no_such_cf"), nullptr);
}

TEST_F(StorageTest, MvccCheckpointCoversAllCF) {
  // Checkpoint 应覆盖 MVCC 三 CF
  WriteBatch batch;
  batch.Put("lock", "ck_k", "lock_v");
  batch.Put("default", "ck_k", "default_v");
  batch.Put("write", "ck_k", "write_v");
  ASSERT_TRUE(storage_->Write(std::move(batch)));

  std::string ck_path = "/tmp/raftkv_mvcc_ck_test_" + std::to_string(getpid());
  fs::remove_all(ck_path);
  ASSERT_TRUE(storage_->CreateCheckpoint(ck_path));

  WriteBatch after;
  after.Put("lock", "after_ck", "should_vanish");
  ASSERT_TRUE(storage_->Write(std::move(after)));

  ASSERT_TRUE(storage_->RestoreFromCheckpoint(ck_path));

  std::string v;
  ASSERT_TRUE(storage_->Get("lock", "ck_k", &v));
  EXPECT_EQ(v, "lock_v");
  ASSERT_TRUE(storage_->Get("default", "ck_k", &v));
  EXPECT_EQ(v, "default_v");
  ASSERT_TRUE(storage_->Get("write", "ck_k", &v));
  EXPECT_EQ(v, "write_v");
  EXPECT_FALSE(storage_->Get("lock", "after_ck", &v));

  fs::remove_all(ck_path);
}

// ── 旧库升级兼容性：老目录只有 default/data/meta 三个 CF ──────────────
TEST(StorageUpgradeTest, OpenLegacyDbAutoCreatesMvccCF) {
  std::string db_path =
      "/tmp/raftkv_legacy_upgrade_test_" + std::to_string(getpid());
  fs::remove_all(db_path);

  // 1. 用裸 RocksDB API 建一个只有 default/data/meta 的"旧库"并写入旧数据
  {
    rocksdb::Options opts;
    opts.create_if_missing = true;
    rocksdb::DB *raw_db = nullptr;
    ASSERT_TRUE(rocksdb::DB::Open(opts, db_path, &raw_db).ok());

    rocksdb::ColumnFamilyHandle *data_h = nullptr;
    rocksdb::ColumnFamilyHandle *meta_h = nullptr;
    ASSERT_TRUE(raw_db
                    ->CreateColumnFamily(rocksdb::ColumnFamilyOptions{},
                                         "data", &data_h)
                    .ok());
    ASSERT_TRUE(raw_db
                    ->CreateColumnFamily(rocksdb::ColumnFamilyOptions{},
                                         "meta", &meta_h)
                    .ok());
    ASSERT_TRUE(raw_db
                    ->Put(rocksdb::WriteOptions{}, data_h, "legacy_key",
                          "legacy_val")
                    .ok());
    raw_db->DestroyColumnFamilyHandle(data_h);
    raw_db->DestroyColumnFamilyHandle(meta_h);
    delete raw_db;
  }

  // 2. 用新 RocksDbStorage 打开：create_missing_column_families 自动补建
  //    lock/write CF，旧数据可读
  RocksDbStorage storage{StorageOptions{db_path}};
  ASSERT_TRUE(storage.Open());

  std::string v;
  ASSERT_TRUE(storage.Get("legacy_key", &v));
  EXPECT_EQ(v, "legacy_val");

  // 新 CF 立即可用
  WriteBatch batch;
  batch.Put("lock", "new_k", "new_v");
  ASSERT_TRUE(storage.Write(std::move(batch)));
  ASSERT_TRUE(storage.Get("lock", "new_k", &v));
  EXPECT_EQ(v, "new_v");

  storage.Close();
  fs::remove_all(db_path);
}

} // namespace raftkv
