# 修复方案：on_apply 写入顺序/时序问题 与 MvccTxn 存储错误传播

## 问题一：`KVStateMachine::on_apply` 的延迟 BatchWrite

### 现状（src/raft/kv_state_machine.cc:33-192）

```cpp
void KVStateMachine::on_apply(braft::Iterator &iter) {
  int64_t last_index = 0;
  std::vector<std::pair<std::string, std::string>> puts;
  std::vector<std::string> deletes;

  for (; iter.valid(); iter.next()) {
    braft::AsyncClosureGuard closure_guard(iter.done());   // ← 每轮循环结束即释放
    ...
    case kv::OP_PUT:    puts.emplace_back(key, value); break;   // 只暂存
    case kv::OP_DELETE: deletes.push_back(key);        break;   // 只暂存
    ...
  }
  if (!puts.empty() || !deletes.empty()) {
    auto s = storage_->BatchWrite(puts, deletes);       // ← 循环结束后才落盘
    ...
  }
}
```

确认存在两个缺陷：

1. **先应答、后落盘**：`AsyncClosureGuard` 在每轮循环末尾析构并 `Run()` closure，leader 上客户端此时就收到成功响应；而 RocksDB 写入发生在整个循环之后。若 closure 触发的读（或 `WaitApplied` 之后的 Log Read barrier 读）先于 `BatchWrite` 执行，会读到旧值——破坏线性一致性。且 `BatchWrite` 失败时客户端已被告知成功。
2. **改变日志顺序**：`RocksDbStorage::BatchWrite` 先执行全部 Put、再执行全部 Delete（rocksdb_storage.cc）。同一批日志若为 `PUT k=v1 → DELETE k → PUT k=v2` 这类交错序列，重排后终态可能与逐条按序应用不同。

另外 `last_applied_index_` 的推进（第 188-191 行）也发生在写盘之前的语义下是错的——推进必须在数据真正落盘之后。

### 修复方案 A（推荐）：保持日志原顺序的单个 WriteBatch + 延迟 closure

利用已有的跨 CF `WriteBatch`（rocksdb_storage.h 的 `raftkv::WriteBatch`，Put/Delete 按 push 顺序进入同一个 `rocksdb::WriteBatch`，天然保序），把 OP_PUT/OP_DELETE 的 closure 用 `closure_guard.release()` 摘出、延迟到批量写成功之后再 `Run()`：

```cpp
// src/raft/kv_state_machine.cc
void KVStateMachine::on_apply(braft::Iterator &iter) {
  int64_t last_index = 0;
  WriteBatch batch;                              // 按日志顺序累积，保序
  std::vector<braft::Closure *> pending_closures; // OP_PUT/OP_DELETE 的延迟 closure

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
    switch (operation.op()) {
    case kv::OP_PUT: {
      batch.Put("data", operation.key(), operation.value());
      if (braft::Closure *done = iter.done()) {
        closure_guard.release(); // 注意：返回 google::protobuf::Closure*，丢弃即可
        pending_closures.push_back(done); // 用 iter.done() 的类型化指针，落盘后再应答
      }
      break;
    }
    case kv::OP_DELETE: {
      batch.Delete("data", operation.key());
      if (braft::Closure *done = iter.done()) {
        closure_guard.release();
        pending_closures.push_back(done);
      }
      break;
    }
    // ... OP_GET / OP_SCAN / OP_TXN_* 分支保持不变 ...
    }
  }

  // 单个 WriteBatch 原子落盘（保持日志原顺序），成功后统一应答。
  bool write_ok = true;
  if (/* batch 非空，见下方 WriteBatch::empty() */ !batch.empty()) {
    auto s = storage_->Write(std::move(batch));
    if (!s) {
      write_ok = false;
      LOG(ERROR) << "on_apply 批量落盘失败: " << s.error_msg;
    }
  }
  for (braft::Closure *done : pending_closures) {
    if (!write_ok) {
      done->status().set_error(EIO, "storage write failed");
    }
    // 与 AsyncClosureGuard 的析构语义保持一致：closure 在 bthread 里运行，
    // 避免阻塞 on_apply 所在的 FSM 线程。
    braft::run_closure_in_bthread(done);
  }

  // 只有真正落盘之后才推进 applied index（Log Read barrier 依赖它）。
  if (write_ok && last_index > 0) {
    last_applied_index_.store(last_index, std::memory_order_release);
    applied_cv_.notify_all();
  }
}
```

配套小改动：给 `raftkv::WriteBatch` 增加 `empty()`（src/storage/rocksdb_storage.h）：

```cpp
class WriteBatch {
public:
  // ... 现有 Put/Delete ...
  bool empty() const { return ops_.empty(); }
  ...
};
```

说明：
- `"data"` CF 与原 `BatchWrite` 目标一致，旧路径行为不变，仅修正顺序与时序。
- `braft::AsyncClosureGuard` 实际是 `std::unique_ptr<google::protobuf::Closure, RunClosureInBthread>`（braft/util.h），因此 `release()` 返回的是 **`google::protobuf::Closure*` 而不是 `braft::Closure*`**——不能直接拿它去 `status().set_error(...)`。正确做法是先用 `iter.done()` 拿到类型化的 `braft::Closure*`（`braft::Closure` 继承自 `google::protobuf::Closure`，二者指向同一对象），再调用 `release()` 仅用于解除 guard 的所有权、丢弃其返回值。
- guard 的析构不是直接 `Run()`，而是 `run_closure_in_bthread(done)`；延迟应答时也应用 `braft::run_closure_in_bthread(done)`（需 `#include <braft/util.h>`），保持相同的执行语义并避免阻塞 FSM 线程。
- 落盘失败时通过 `done->status().set_error(...)` 把错误传回客户端，而不是假装成功。
- 存储写失败属于不可恢复错误；更严格的做法是同时调用 `iter.set_error_and_rollback()` 让节点停机进入错误状态，避免状态机与日志脱节，可按需追加。

### 备选方案 B：逐条立即落盘

每条 OP_PUT/OP_DELETE 在 closure 返回前直接 `storage_->Put/Delete`，实现最简单，但每条日志一次 RocksDB 写，批量吞吐低于方案 A，不推荐在高写入场景使用：

```cpp
case kv::OP_PUT: {
  auto s = storage_->Put(operation.key(), operation.value());
  if (!s && iter.done() != nullptr) {
    iter.done()->status().set_error(EIO, s.error_msg);
  }
  break; // closure_guard 析构时 Run()，此时数据已落盘
}
case kv::OP_DELETE: {
  auto s = storage_->Delete(operation.key());
  if (!s && iter.done() != nullptr) {
    iter.done()->status().set_error(EIO, s.error_msg);
  }
  break;
}
```

（方案 B 下 `last_applied_index_` 的推进同样应保证在写盘之后——逐条落盘时循环末尾推进即可。）

---

## 问题二：`MvccTxn::Prewrite/Commit/Rollback` 未检查 `storage_->Write` 返回值

### 现状（src/storage/mvcc_txn.cc）

三处均为：

```cpp
storage_->Write(std::move(batch));   // 返回的 Status 被丢弃
return MvccError::Ok();
```

RocksDB 写失败（磁盘满、IO 错误等）时仍向客户端返回成功，且各副本状态可能悄然分叉。

### 修复

1. 给 `MvccError` 增加存储错误类型（src/storage/mvcc_txn.h）：

```cpp
struct MvccError {
  enum Kind {
    kNone = 0,
    kLocked,
    kWriteConflict,
    kTxnNotFound,
    kAlreadyRolledBack,
    kStorageError,     // 新增：RocksDB 写入失败
  };
  ...
};
```

2. 三个写方法统一检查并传播（src/storage/mvcc_txn.cc）：

```cpp
namespace {
MvccError StorageError(const std::string &key, const Status &s) {
  MvccError err;
  err.kind = MvccError::kStorageError;
  err.key = key;
  err.message = "storage write failed: " + s.error_msg;
  return err;
}
} // namespace

// Prewrite 末尾：
  WriteBatch batch;
  batch.Put(KLockCF, key, lock.Serialize());
  if (!is_delete) {
    batch.Put(kDefaultCF, MvccCodec::EncodeKey(key, start_ts), value);
  }
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();

// Commit 末尾：
  WriteBatch batch;
  batch.Put(kWriteCF, MvccCodec::EncodeKey(key, commit_ts), write.Serialize());
  batch.Delete(KLockCF, key);
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();

// Rollback 末尾：
  batch.Put(kWriteCF, MvccCodec::EncodeKey(key, start_ts), tomb.Serialize());
  if (auto s = storage_->Write(std::move(batch)); !s) {
    return StorageError(key, s);
  }
  return MvccError::Ok();
```

3. 调用链无需改动：`on_apply` 的 OP_TXN_* 分支已经把 `MvccError` 写回 response（`FillTxnError`），`kStorageError` 会自然作为 `success=false` 传给客户端。`CheckTxnStatus` 内部对 `Rollback` 的返回值已有检查（mvcc_txn.cc:307-310、318-321），会自动传播新错误类型。

### 备注

- 存储写失败在 Raft 状态机里是严重事件（副本可能与日志脱节）。除返回错误外，建议在 `on_apply` 遇到 `kStorageError` 时额外 `LOG(FATAL)` 或 `iter.set_error_and_rollback()`，与问题一的处理策略保持一致。
- `Rollback` 中间的 `storage_->Get(KLockCF, ...)` 失败被当作"锁不存在"（rocksdb_storage.h 注释已说明该约定），本次不改；如需更严格可让 MVCC 层的 `Get` 也返回三态。
