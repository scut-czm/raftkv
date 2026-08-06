// tests/chaos/mvcc_db_check.cc
// 离线 MVCC 数据一致性检查器：集群停止后直接打开节点的 RocksDB，检查
//   1. lock CF 必须为空（无残锁泄漏）；
//   2. write CF 无悬空 start_ts：每条 kPut 提交记录指向的
//      default CF 数据版本 EncodeKey(key, start_ts) 必须存在，
//      且 commit_ts > start_ts；kDelete/kRollback 不要求数据版本。
//
// 用法：./mvcc_db_check --db_path=/tmp/raftkv_data_8200/rocksdb
// 退出码：0 = 通过；1 = 存在违规；2 = 打开 DB 失败。

#include <cstdint>
#include <cstdio>
#include <gflags/gflags.h>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "storage/mvcc_codec.h"

DEFINE_string(db_path, "", "RocksDB 目录（如 /tmp/raftkv_data_8200/rocksdb）");

namespace {

// 可打印的 key 摘要（二进制安全）
std::string Hex(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      out.push_back((char)c);
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

} // namespace

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_db_path.empty()) {
    fprintf(stderr, "用法: mvcc_db_check --db_path=<rocksdb目录>\n");
    return 2;
  }

  // 列出全部 CF 并只读打开
  rocksdb::Options options;
  std::vector<std::string> cf_names;
  auto s = rocksdb::DB::ListColumnFamilies(options, FLAGS_db_path, &cf_names);
  if (!s.ok()) {
    fprintf(stderr, "ListColumnFamilies 失败: %s\n", s.ToString().c_str());
    return 2;
  }
  std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
  for (const auto &name : cf_names) {
    descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
  }
  rocksdb::DB *db = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  s = rocksdb::DB::OpenForReadOnly(rocksdb::DBOptions(), FLAGS_db_path,
                                   descriptors, &handles, &db);
  if (!s.ok()) {
    fprintf(stderr, "OpenForReadOnly 失败: %s\n", s.ToString().c_str());
    return 2;
  }

  rocksdb::ColumnFamilyHandle *lock_cf = nullptr;
  rocksdb::ColumnFamilyHandle *write_cf = nullptr;
  rocksdb::ColumnFamilyHandle *default_cf = nullptr;
  for (size_t i = 0; i < cf_names.size(); ++i) {
    if (cf_names[i] == "lock") lock_cf = handles[i];
    if (cf_names[i] == "write") write_cf = handles[i];
    if (cf_names[i] == "default") default_cf = handles[i];
  }
  if (!lock_cf || !write_cf || !default_cf) {
    fprintf(stderr, "缺少 lock/write/default CF（不是 MVCC 库？）\n");
    return 2;
  }

  int violations = 0;
  rocksdb::ReadOptions ro;

  // ── 检查 1：lock CF 应为空 ────────────────────────────────────────
  uint64_t lock_count = 0;
  {
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, lock_cf));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++lock_count;
      auto info = raftkv::LockInfo::Deserialize(
          std::string_view(it->value().data(), it->value().size()));
      fprintf(stderr, "[残锁] key=%s primary=%s start_ts=%llu ttl=%llums\n",
              Hex({it->key().data(), it->key().size()}).c_str(),
              info ? info->primary.c_str() : "?",
              info ? (unsigned long long)info->start_ts : 0ULL,
              info ? (unsigned long long)info->ttl_ms : 0ULL);
    }
  }
  if (lock_count > 0) {
    ++violations;
    fprintf(stderr, "FAIL: lock CF 残留 %llu 把锁\n",
            (unsigned long long)lock_count);
  } else {
    printf("PASS: lock CF 为空（无残锁泄漏）\n");
  }

  // ── 检查 2：write CF 无悬空 start_ts ─────────────────────────────
  uint64_t write_total = 0, put_cnt = 0, del_cnt = 0, rb_cnt = 0,
           dangling = 0, bad_order = 0, undecodable = 0;
  {
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, write_cf));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++write_total;
      std::string_view user_key;
      uint64_t commit_ts = 0;
      if (!raftkv::MvccCodec::DecodeKey(
              {it->key().data(), it->key().size()}, &user_key, &commit_ts)) {
        ++undecodable;
        ++violations;
        fprintf(stderr, "[坏键] write CF 键无法解码: %s\n",
                Hex({it->key().data(), it->key().size()}).c_str());
        continue;
      }
      auto info = raftkv::WriteInfo::Deserialize(
          std::string_view(it->value().data(), it->value().size()));
      if (!info) {
        ++undecodable;
        ++violations;
        fprintf(stderr, "[坏值] write CF 值无法解码: key=%s\n",
                Hex(user_key).c_str());
        continue;
      }
      switch (info->kind) {
      case raftkv::WriteInfo::kPut: {
        ++put_cnt;
        if (info->start_ts >= commit_ts) {
          ++bad_order;
          ++violations;
          fprintf(stderr,
                  "[时序错] key=%s start_ts=%llu >= commit_ts=%llu\n",
                  Hex(user_key).c_str(),
                  (unsigned long long)info->start_ts,
                  (unsigned long long)commit_ts);
        }
        // 悬空检查：提交记录指向的数据版本必须存在
        std::string data_key =
            raftkv::MvccCodec::EncodeKey(user_key, info->start_ts);
        std::string value;
        auto gs = db->Get(ro, default_cf, data_key, &value);
        if (!gs.ok()) {
          ++dangling;
          ++violations;
          fprintf(stderr,
                  "[悬空] key=%s commit_ts=%llu → start_ts=%llu "
                  "在 default CF 无数据版本\n",
                  Hex(user_key).c_str(), (unsigned long long)commit_ts,
                  (unsigned long long)info->start_ts);
        }
        break;
      }
      case raftkv::WriteInfo::kDelete:
        ++del_cnt;
        break;
      case raftkv::WriteInfo::kRollback:
        ++rb_cnt;
        break;
      }
    }
  }
  printf("write CF 共 %llu 条：Put=%llu Delete=%llu Rollback=%llu\n",
         (unsigned long long)write_total, (unsigned long long)put_cnt,
         (unsigned long long)del_cnt, (unsigned long long)rb_cnt);
  if (dangling == 0 && bad_order == 0 && undecodable == 0) {
    printf("PASS: write CF 无悬空 start_ts、无时序错误\n");
  }

  for (auto *h : handles) {
    db->DestroyColumnFamilyHandle(h);
  }
  delete db;

  if (violations > 0) {
    fprintf(stderr, "FAIL: 共 %d 处违规\n", violations);
    return 1;
  }
  printf("ALL PASS: %s\n", FLAGS_db_path.c_str());
  return 0;
}
