#include "storage/rocksdb_storage.h"

#include <filesystem>
#include <memory>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_batch.h>
#include <vector>

namespace raftkv {

namespace fs = std::filesystem;

RocksDbStorage::RocksDbStorage(StorageOptions options)
    : options_(std::move(options)) {
  write_opts_.disableWAL = options_.disable_wal;
}

RocksDbStorage::~RocksDbStorage() { Close(); }

Status RocksDbStorage::Open() {
  rocksdb::DBOptions db_opts;
  db_opts.create_if_missing = true;
  db_opts.create_missing_column_families = true;
  db_opts.max_background_jobs = options_.max_background_jobs;

  // ── Block Cache（全局共享）──
  auto cache = rocksdb::NewLRUCache(options_.block_cache_size);

  // ── Block-Based Table 配置 ──
  rocksdb::BlockBasedTableOptions table_opts;
  table_opts.block_cache = cache;
  table_opts.block_size = 16 * 1024;
  table_opts.cache_index_and_filter_blocks = true;
  table_opts.pin_l0_filter_and_index_blocks_in_cache = true;
  table_opts.filter_policy.reset(
      rocksdb::NewBloomFilterPolicy(options_.bloom_bits_per_key, false));

  // data CF 配置
  rocksdb::ColumnFamilyOptions data_cf_opts;
  data_cf_opts.write_buffer_size = options_.write_buffer_size;
  data_cf_opts.max_write_buffer_number = options_.max_write_buffer_number;
  data_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_opts));
  data_cf_opts.compression_per_level = {
      rocksdb::kNoCompression,  rocksdb::kNoCompression,
      rocksdb::kLZ4Compression, rocksdb::kLZ4Compression,
      rocksdb::kLZ4Compression, rocksdb::kLZ4Compression,
      rocksdb::kLZ4Compression,
  };
  data_cf_opts.level_compaction_dynamic_level_bytes = true;
  data_cf_opts.target_file_size_base = 64 * 1024 * 1024;
  data_cf_opts.level0_slowdown_writes_trigger = 40;
  data_cf_opts.level0_stop_writes_trigger = 80;

  // meta CF 配置（元数据量小，使用默认配置即可）
  rocksdb::ColumnFamilyOptions meta_cf_opts;

  // ── MVCC 三 CF 配置 ──────────────────────────────────────────────────
  // default CF：MVCC 数据版本（EncodeKey(key, start_ts) -> value）。
  // 数据体量与 data CF 同级，直接复用 data CF 的重载配置。
  rocksdb::ColumnFamilyOptions mvcc_default_cf_opts = data_cf_opts;

  // lock CF：只存未提交事务的锁，事务提交/回滚后即删除。
  // 常驻数据量极小、读极频繁（每次快照读都要查锁）——
  // 小 write buffer + 全内存友好即可，不需要大缓存和分层压缩。
  rocksdb::ColumnFamilyOptions lock_cf_opts;
  lock_cf_opts.write_buffer_size = 16 * 1024 * 1024;
  lock_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_opts));

  // write CF：提交记录（EncodeKey(key, commit_ts) -> WriteInfo）。
  // 读路径以 Seek 为主（SeekWrite），bloom filter 对 Seek 无效前缀场景
  // 收益有限，但 whole-key bloom 仍能加速 GetTxnRecord 的点查，保留即可。
  rocksdb::ColumnFamilyOptions write_cf_opts;
  write_cf_opts.write_buffer_size = options_.write_buffer_size;
  write_cf_opts.max_write_buffer_number = options_.max_write_buffer_number;
  write_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_opts));
  write_cf_opts.level_compaction_dynamic_level_bytes = true;

  std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors = {
      {rocksdb::kDefaultColumnFamilyName, mvcc_default_cf_opts},
      {kDataCF, data_cf_opts},
      {kMetaCF, meta_cf_opts},
      {kLockCF, lock_cf_opts},
      {kWriteCF, write_cf_opts},
  };

  std::vector<rocksdb::ColumnFamilyHandle *> handles;

  rocksdb::DB *db = nullptr;
  auto s = rocksdb::DB::Open(db_opts, options_.db_path, cf_descriptors,
                             &handles, &db);
  if (!s.ok()) {
    return Status::Error("RocksDB Open failed: " + s.ToString());
  }
  db_ = db;
  default_cf_handle_ = handles[0];
  data_cf_handle_ = handles[1];
  meta_cf_handle_ = handles[2];
  lock_cf_handle_ = handles[3];
  write_cf_handle_ = handles[4];

  // ── 保存写选项 ──
  write_opts_.disableWAL = options_.disable_wal;
  return Status::OK();
}

void RocksDbStorage::Close() {
  if (db_ == nullptr) {
    return;
  }

  for (auto **h : {&default_cf_handle_, &data_cf_handle_, &meta_cf_handle_,
                   &lock_cf_handle_, &write_cf_handle_}) {
    if (*h) {
      db_->DestroyColumnFamilyHandle(*h);
      *h = nullptr;
    }
  }
  delete db_;
  db_ = nullptr;
}
bool RocksDbStorage::IsOpen() const { return db_ != nullptr; }

// ── 基本操作 ──────────────────────────────────────────────────────────────
Status RocksDbStorage::Put(const std::string &key, const std::string &value) {

  auto s = db_->Put(write_opts_, data_cf_handle_, key, value);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

Status RocksDbStorage::Get(const std::string &key, std::string *value) const {
  auto s = db_->Get(rocksdb::ReadOptions{}, data_cf_handle_, key, value);
  if (s.IsNotFound()) {
    return Status::Error("not found");
  }
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

Status RocksDbStorage::Delete(const std::string &key) {
  auto s = db_->Delete(write_opts_, data_cf_handle_, key);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

Status RocksDbStorage::BatchWrite(
    const std::vector<std::pair<std::string, std::string>> &puts,
    const std::vector<std::string> &deletes) {
  rocksdb::WriteBatch batch;
  for (const auto &[key, value] : puts) {
    batch.Put(data_cf_handle_, key, value);
  }
  for (const auto &key : deletes) {
    batch.Delete(data_cf_handle_, key);
  }
  auto s = db_->Write(write_opts_, &batch);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

std::vector<std::pair<std::string, std::string>>
RocksDbStorage::Scan(const std::string &start_key, const std::string &end_key,
                     int limit) const {
  rocksdb::ReadOptions read_opts;
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(read_opts, data_cf_handle_));
  std::vector<std::pair<std::string, std::string>> results;
  it->Seek(start_key);
  int count = 0;
  for (; it->Valid(); it->Next()) {
    if (limit > 0 && count >= limit) {
      break;
    }
    std::string key = it->key().ToString();
    if (!end_key.empty() && key >= end_key) {
      break;
    }
    results.emplace_back(key, it->value().ToString());
    ++count;
  }
  return results;
}

// ── 快照 ──────────────────────────────────────────────────────────────────

Status RocksDbStorage::CreateCheckpoint(const std::string &checkpoint_path) {
  rocksdb::Checkpoint *checkpoint = nullptr;
  auto s = rocksdb::Checkpoint::Create(db_, &checkpoint);
  if (!s.ok()) {
    return Status::Error("Checkpoint::Create failed: " + s.ToString());
  }
  s = checkpoint->CreateCheckpoint(checkpoint_path);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

Status
RocksDbStorage::RestoreFromCheckpoint(const std::string &checkpoint_path) {
  // 关闭当前数据库
  Close();

  // 清空旧数据目录，将 checkpoint 目录直接用作新数据目录
  // 策略：先备份，再替换（checkpoint 本身就是完整的 RocksDB 目录）
  std::error_code ec;
  fs::remove_all(options_.db_path, ec);
  if (ec) {
    return Status::Error("remove db_path failed: " + ec.message());
  }
  fs::copy(checkpoint_path, options_.db_path, fs::copy_options::recursive, ec);
  if (ec) {
    return Status::Error("copy checkpoint failed: " + ec.message());
  }
  return Open();
}

// ── meta CF 操作 ──────────────────────────────────────────────────────────
Status RocksDbStorage::PutMeta(const std::string &key,
                               const std::string &value) {
  auto s = db_->Put(rocksdb::WriteOptions{}, meta_cf_handle_, key, value);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}
Status RocksDbStorage::GetMeta(const std::string &key,
                               std::string *value) const {
  auto s = db_->Get(rocksdb::ReadOptions{}, meta_cf_handle_, key, value);
  if (s.IsNotFound())
    return Status::Error("not found");
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}
// ── MVCC 通用接口 ─────────────────────────────────────────────────────────
rocksdb::ColumnFamilyHandle *
RocksDbStorage::ResolveHandle(const std::string &cf) const {
  if (cf == rocksdb::kDefaultColumnFamilyName) {
    return default_cf_handle_;
  }
  if (cf == kDataCF) {
    return data_cf_handle_;
  }
  if (cf == kMetaCF) {
    return meta_cf_handle_;
  }
  if (cf == kLockCF) {
    return lock_cf_handle_;
  }
  if (cf == kWriteCF) {
    return write_cf_handle_;
  }
  return nullptr;
}

bool RocksDbStorage::Get(const std::string &cf, const std::string &key,
                         std::string *value) const {
  auto *handle = ResolveHandle(cf);
  if (handle == nullptr) {
    return false;
  }
  auto s = db_->Get(rocksdb::ReadOptions{}, handle, key, value);
  return s.ok();
}

std::unique_ptr<rocksdb::Iterator>
RocksDbStorage::NewIterator(const std::string &cf) const {
  auto *handle = ResolveHandle(cf);
  if (handle == nullptr) {
    return nullptr;
  }
  return std::unique_ptr<rocksdb::Iterator>(
      db_->NewIterator(rocksdb::ReadOptions{}, handle));
}

Status RocksDbStorage::Write(WriteBatch &&batch) {
  rocksdb::WriteBatch rocks_batch;
  for (const auto &op : batch.ops_) {
    auto *handle = ResolveHandle(op.cf);
    if (handle == nullptr) {
      return Status::Error("unknown column family: " + op.cf);
    }
    if (op.type == WriteBatch::OpType::kDelete) {
      rocks_batch.Delete(handle, op.key);
    } else {
      rocks_batch.Put(handle, op.key, op.value);
    }
  }
  auto s = db_->Write(write_opts_, &rocks_batch);
  return s.ok() ? Status::OK() : Status::Error(s.ToString());
}

} // namespace raftkv