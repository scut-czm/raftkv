#include "raft/kv_state_machine.h"

#include <atomic>
#include <braft/raft.h>
#include <butil/iobuf.h>
#include <butil/logging.h>
#include <cstdint>
#include <memory>
#include <string>

#include "braft/storage.h"
#include "braft/util.h"
#include "kv.pb.h"
#include "storage/rocksdb_storage.h"

namespace raftkv {
KVStateMachine::KVStateMachine(std::shared_ptr<RocksDbStorage> storage)
    : storage_(std::move(storage)) {}

// ── on_apply：核心函数，应用提交的日志 ──────────────────────────────────
void KVStateMachine::on_apply(braft::Iterator &iter) {
  int64_t last_index = 0;
  std::vector<std::pair<std::string, std::string>> puts;
  std::vector<std::string> deletes;

  for (; iter.valid(); iter.next()) {
    braft::AsyncClosureGuard closure_guard(iter.done());

    int64_t log_index = iter.index();
    last_index = iter.index();
    butil::IOBuf data = iter.data();

    kv::KvOperation operation;
    butil::IOBufAsZeroCopyInputStream wrapper(data);
    if (!operation.ParseFromZeroCopyStream(&wrapper)) {
      LOG(ERROR) << "解析 KvOperation 失败 [index=" << log_index << "]";
      continue;
    }
    // RocksDB 内部有锁，on_apply 串行执行，无需外层 mutex
    switch (operation.op()) {
    case kv::OP_PUT: {
      puts.emplace_back(operation.key(), operation.value());
      break;
    }
    case kv::OP_DELETE: {
      deletes.push_back(operation.key());
      break;
    }
    case kv::OP_GET: {
      // 线性一致读：日志仅用于建立 apply 顺序点，实际读在
      // ReadIndexClosure::Run()
      VLOG(1) << "GET [index=" << log_index << ", key=" << operation.key()
              << "]";
      break;
    }
    case kv::OP_SCAN: {
      // 线性一致 Scan：同上，实际读在 ScanReadIndexClosure::Run()
      VLOG(1) << "SCAN [index=" << log_index << "]";
      break;
    }
    default:
      LOG(WARNING) << "未知操作 [index=" << log_index
                   << ", op=" << operation.op() << "]";
      break;
    }
  }
  if (!puts.empty() || !deletes.empty()) {
    auto s = storage_->BatchWrite(puts, deletes);
    if (!s) {
      LOG(ERROR) << "BatchWrite 失败: " << s.error_msg;
    }
  }
  if (last_index > 0) {
    last_applied_index_.store(last_index, std::memory_order_release);
    applied_cv_.notify_all();
  }
}

// ── on_snapshot_save：RocksDB Checkpoint（硬链接，毫秒级）─────────────
void KVStateMachine::on_snapshot_save(braft::SnapshotWriter *writer,
                                      braft::Closure *done) {
  std::string checkpoint_path = writer->get_path() + "/checkpoint";
  auto s = storage_->CreateCheckpoint(checkpoint_path);
  if (!s) {
    LOG(ERROR) << "CreateCheckpoint 失败: " << s.error_msg;
    done->status().set_error(EIO, "CreateCheckpoint failed");
    done->Run();
    return;
  }
  // 告诉 braft 快照包含 checkpoint/ 整个目录
  if (writer->add_file("checkpoint/") != 0) {
    LOG(ERROR) << "add_file checkpoint/ 失败";
    done->status().set_error(EIO, "add_file failed");
    done->Run();
    return;
  }
  LOG(INFO) << "Snapshot saved via Checkpoint: " << checkpoint_path;
  done->Run();
}

// ── on_snapshot_load：从 Checkpoint 目录恢复 ─────────────────────────
int KVStateMachine::on_snapshot_load(braft::SnapshotReader *reader) {
  if (reader->get_file_meta("checkpoint/", nullptr) != 0) {
    LOG(ERROR) << "Snapshot 中不包含 checkpoint/ 目录";
    return -1;
  }
  std::string checkpoint_path = reader->get_path() + "/checkpoint";
  auto s = storage_->RestoreFromCheckpoint(checkpoint_path);
  if (!s) {
    LOG(ERROR) << "RestoreFromCheckpoint 失败: " << s.error_msg;
    return -1;
  }
  LOG(INFO) << "Snapshot loaded from Checkpoint: " << checkpoint_path;
  return 0;
}
// ── Leader 变更通知 ───────────────────────────────────────────────────
void KVStateMachine::on_leader_start(int64_t term) {
  LOG(INFO) << "=== Became LEADER at term=" << term << " ===";
  leader_term_.store(term, std::memory_order_release);
  is_leader_.store(true, std::memory_order_release);
}
void KVStateMachine::on_leader_stop(const butil::Status &status) {
  LOG(INFO) << "=== Stopped being LEADER: " << status << " ===";
  leader_term_.store(-1, std::memory_order_release);
  is_leader_.store(false, std::memory_order_release);
}

// ── 业务读接口（不走 Raft）────────────────────────────────────────────
bool KVStateMachine::Get(const std::string &key, std::string *value) const {
  return storage_->Get(key, value).ok;
}

std::vector<std::pair<std::string, std::string>>
KVStateMachine::Scan(const std::string &start_key, const std::string &end_key,
                     int limit) const {
  return storage_->Scan(start_key, end_key, limit);
}

bool KVStateMachine::WaitApplied(int64_t target, int64_t timeout_ms) const {
  if (last_applied_index_.load(std::memory_order_acquire) >= target) {
    return true;
  }
  std::unique_lock<std::mutex> lock(applied_mutex_);
  return applied_cv_.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), [this, target] {
        return last_applied_index_.load(std::memory_order_acquire) >= target;
      });
}
} // namespace raftkv