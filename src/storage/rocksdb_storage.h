#pragma once

#include <memory>
#include <rocksdb/db.h>

#include <rocksdb/iterator.h>
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
  bool disable_wal = true; // 新增，braft 已保证持久性
};

// 跨 CF 原子写：按 CF 名记录操作，交给 RocksDbStorage::Write 时
// 统一解析成 handle 后写进同一个 rocksdb::WriteBatch。
// Percolator 的 Prewrite/Commit/Rollback 都依赖「lock + default + write
// 三个 CF 的变更要么全部落盘要么全不落盘」，这就是原子性的来源。
class WriteBatch {
public:
  void Put(const std::string &cf, const std::string &key,
           const std::string &value) {
    ops_.push_back({OpType::kPut, cf, key, value});
  }

  void Delete(const std::string &cf, const std::string &key) {
    ops_.push_back({OpType::kDelete, cf, key, ""});
  }

  bool empty() const { return ops_.empty(); }

private:
  friend class RocksDbStorage;
  enum class OpType { kPut, kDelete };
  struct Op {
    OpType type;
    std::string cf;
    std::string key;
    std::string value;
  };

  std::vector<Op> ops_;
};

// RocksDbStorage：RaftKV 的存储后端
// 使用两个 Column Family：data（用户数据）、meta（元数据）
// Raft 日志由 braft 接管，不需要 log CF

// 新RocksDbStorage：RaftKV 的存储后端
//
// 五个 Column Family：
//   data    非事务用户数据（旧路径，保持向后兼容）
//   meta    节点元数据
//   default MVCC 数据版本    EncodeKey(key, start_ts)  -> value
//   lock    未提交事务的锁   key                       -> LockInfo
//   write   已提交/已回滚记录 EncodeKey(key, commit_ts) -> WriteInfo
//
// Raft 日志由 braft 接管，不需要 log CF。
// 旧库升级：Open 时 create_missing_column_families=true 会自动补建
// lock/write CF；data CF 里的旧数据不受影响，继续走原有非事务路径。
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

  // 基本 KV 操作（操作 data CF，非事务旧路径）
  Status Put(const std::string &key, const std::string &value);
  Status Get(const std::string &key, std::string *value) const;
  Status Delete(const std::string &key);

  // 批量写入（原子性，data CF）
  Status
  BatchWrite(const std::vector<std::pair<std::string, std::string>> &puts,
             const std::vector<std::string> &deletes);

  // 范围扫描（字典序，[start_key, end_key)，limit=0 不限制 data CF）
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

  // 快照：基于 RocksDB Checkpoint（硬链接，毫秒级）
  // Checkpoint 天然覆盖全部 CF，MVCC 三 CF 无需额外处理。
  Status CreateCheckpoint(const std::string &checkpoint_path);
  Status RestoreFromCheckpoint(const std::string &checkpoint_path);

  // meta CF 操作（存储节点元数据）
  Status PutMeta(const std::string &key, const std::string &value);
  Status GetMeta(const std::string &key, std::string *value) const;

  // ── MVCC 通用接口（按 CF 名操作，供 MvccTxn 使用）────────────────────
  // 返回 true = 找到；false = 不存在（其它错误视为不存在，MVCC 层有断言兜底）
  bool Get(const std::string &cf, const std::string &key,
           std::string *value) const;

  // 在指定 CF 上建迭代器（MvccTxn::SeekWrite / Scan 用）
  std::unique_ptr<rocksdb::Iterator> NewIterator(const std::string &cf) const;

  // 跨 CF 原子写入（Percolator 单条命令的全部落盘动作走这里）
  Status Write(WriteBatch &&batch);

private:
  rocksdb::ColumnFamilyHandle *GetDataHandle() const;
  rocksdb::ColumnFamilyHandle *GetMetaHandle() const;
  static constexpr const char *kDataCF = "data";
  static constexpr const char *kMetaCF = "meta";

  rocksdb::ColumnFamilyHandle *ResolveHandle(const std::string &cf) const;
  static constexpr const char *kLockCF = "lock";
  static constexpr const char *kWriteCF = "write";

  StorageOptions options_;
  rocksdb::DB *db_ = nullptr;
  rocksdb::WriteOptions write_opts_;
  rocksdb::ColumnFamilyHandle *default_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *data_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *meta_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *lock_cf_handle_ = nullptr;
  rocksdb::ColumnFamilyHandle *write_cf_handle_ = nullptr;
};

} // namespace raftkv
