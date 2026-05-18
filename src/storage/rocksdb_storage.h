#pragma once

#include <memory>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/checkpoint.h>
#include <string>
#include <utility>
#include <vector>

namespace raftkv {

// 操作结果
struct Status {
  bool ok;
  std::string error_msg;
  static Status OK() { return {true, ""}; }
  static Status Error(const std::string &msg) { return {false, msg}; }
  explicit operator bool() const { return ok; }
};

struct StorageOptions {
  std::string db_path;

  // ── MemTable ──
  size_t write_buffer_size = 64 * 1024 * 1024; // 64MB
  int max_write_buffer_number = 4; // 3 → 4，多留一个 buffer 避免写停顿

  // ── 后台线程 ──
  int max_background_jobs = 8; // 4 → 8（flush + compaction 并行）

  // ── Block Cache ──
  size_t block_cache_size = 256 * 1024 * 1024; // 256MB（新增）,不写默认8MB

  // ── Bloom Filter ──
  int bloom_bits_per_key = 10; // 新增，10 bits ≈ 1% 假阳性

  // ── WAL ──
  bool disable_wal=true; //新增，braft 已保证持久性
};

// RocksDbStorage：RaftKV 的存储后端
// 使用两个 Column Family：data（用户数据）、meta（元数据）
// Raft 日志由 braft 接管，不需要 log CF

class RocksDbStorage {
public:
  explicit RocksDbStorage(StorageOptions options);
  ~RocksDbStorage();

  RocksDbStorage(const RocksDbStorage &) = delete;
  RocksDbStorage &operator=(const RocksDbStorage &) = delete;

  // 打开/关闭
  Status Open();
  void Close();
  bool IsOpen() const;

  // 基本 KV 操作（操作 data CF）
  Status Put(const std::string &key, const std::string &value);
  Status Get(const std::string &key, std::string *value) const;
  Status Delete(const std::string &key);

  // 批量写入（原子性）
  Status
  BatchWrite(const std::vector<std::pair<std::string, std::string>> &puts,
             const std::vector<std::string> &deletes);

  // 范围扫描（字典序，[start_key, end_key)，limit=0 不限制）
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

  // 快照：基于 RocksDB Checkpoint（硬链接，毫秒级）
  Status CreateCheckpoint(const std::string &checkpoint_path);
  Status RestoreFromCheckpoint(const std::string &checkpoint_path);

  // meta CF 操作（存储节点元数据）
  Status PutMeta(const std::string &key, const std::string &value);
  Status GetMeta(const std::string &key, std::string *value) const;

private:
  rocksdb::ColumnFamilyHandle *GetDataHandle() const;
  rocksdb::ColumnFamilyHandle *GetMetaHandle() const;
  static constexpr const char *kDataCF = "data";
  static constexpr const char *kMetaCF = "meta";

  StorageOptions options_;
  rocksdb::DB *db_ = nullptr;
  rocksdb::WriteOptions write_opts_;
  rocksdb::ColumnFamilyHandle *default_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *data_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *meta_cf_handle_ = nullptr;
};

} // namespace raftkv
