# MVCC `Get/Scan` 接入 `snapshot_ts` 完整修改代码

本文档基于当前 `raftkv` 工作区中的以下文件编写：

- `proto/kv.proto`
- `src/raft/kv_state_machine.h`
- `src/raft/kv_state_machine.cc`
- `src/service/kv_service.h`
- `src/service/kv_service.cc`

目标：

1. `snapshot_ts == 0`：保持原有非事务路径，读取 `data` CF。
2. `snapshot_ts > 0`：调用 `MvccTxn::Get/Scan`，读取 MVCC 的 `lock/write/default` CF。
3. 因当前没有 follower safe-ts，事务快照读自动按线性一致读处理，强制经过 leader 和 applied barrier。
4. Get/Scan 遇到未提交锁时，返回结构化的 `key/primary/lock_ts`，供客户端调用 `CheckTxnStatus`。
5. 不修改原有 `Put/Delete` 语义。

---

## 一、修改 `proto/kv.proto`

### 1. 在 `GetResponse` 之前新增公共锁冲突消息

```proto
// MVCC 快照读遇到未提交锁时返回。
// 客户端应使用 primary + lock_ts 调用 CheckTxnStatus，
// 再根据返回结果重试读取或推进 secondary。
message LockConflict {
    string key = 1;
    string primary = 2;
    uint64 lock_ts = 3;
}
```

### 2. 用下面代码完整替换现有 `GetResponse`

```proto
message GetResponse {
    bool success = 1;
    bytes value = 2;
    bool found = 3;
    string redirect = 4;
    string error = 5;

    // 仅 snapshot_ts > 0 且读到未提交锁时设置。
    LockConflict lock_conflict = 6;
}
```

### 3. 用下面代码完整替换现有 `ScanResponse`

```proto
message ScanResponse {
    bool success = 1;
    repeated KvPair kvs = 2;
    string redirect = 3;
    string error = 4;

    // Scan 遇到的第一个未提交锁。
    LockConflict lock_conflict = 5;
}
```

字段只追加、不修改原编号，保持 protobuf 向后兼容。

---

## 二、修改 `src/raft/kv_state_machine.h`

### 1. 在标准库 include 区增加

```cpp
#include <optional>
```

### 2. 在现有 `Get` 声明后增加事务快照读接口

```cpp
  // 旧非事务读：读取 data CF。
  bool Get(const std::string &key, std::string *value) const;

  // MVCC 快照读：读取 lock/write/default CF。
  MvccError TxnGet(const std::string &key, uint64_t snapshot_ts,
                   std::optional<std::string> *value) const;
```

### 3. 用下面代码替换现有 `Scan` 声明区域

```cpp
  // 旧非事务范围读：读取 data CF。
  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

  // MVCC 快照范围读：读取 write/default/lock CF。
  MvccError
  TxnScan(const std::string &start_key, const std::string &end_key, int limit,
          uint64_t snapshot_ts,
          std::vector<std::pair<std::string, std::string>> *out) const;
```

修改后的业务接口区域应为：

```cpp
  // ── 业务接口（直接读状态机，不走 Raft）──
  bool Get(const std::string &key, std::string *value) const;

  MvccError TxnGet(const std::string &key, uint64_t snapshot_ts,
                   std::optional<std::string> *value) const;

  bool IsLeader() const { return is_leader_.load(std::memory_order_acquire); }
  int64_t LeaderTerm() const {
    return leader_term_.load(std::memory_order_acquire);
  }

  int64_t LastAppliedIndex() const {
    return last_applied_index_.load(std::memory_order_acquire);
  }

  bool WaitApplied(int64_t target, int64_t timeout_ms) const;

  std::vector<std::pair<std::string, std::string>>
  Scan(const std::string &start_key, const std::string &end_key,
       int limit) const;

  MvccError
  TxnScan(const std::string &start_key, const std::string &end_key, int limit,
          uint64_t snapshot_ts,
          std::vector<std::pair<std::string, std::string>> *out) const;
```

---

## 三、修改 `src/raft/kv_state_machine.cc`

在现有业务读接口区域，用下面代码完整替换当前的 `Get/Scan` 实现；`WaitApplied` 保持原实现不变。

```cpp
// ── 业务读接口（不走 Raft）─────────────────────────────────────────────
bool KVStateMachine::Get(const std::string &key, std::string *value) const {
  return storage_->Get(key, value).ok;
}

MvccError
KVStateMachine::TxnGet(const std::string &key, uint64_t snapshot_ts,
                       std::optional<std::string> *value) const {
  return mvcc_.Get(key, snapshot_ts, value);
}

std::vector<std::pair<std::string, std::string>>
KVStateMachine::Scan(const std::string &start_key, const std::string &end_key,
                     int limit) const {
  return storage_->Scan(start_key, end_key, limit);
}

MvccError KVStateMachine::TxnScan(
    const std::string &start_key, const std::string &end_key, int limit,
    uint64_t snapshot_ts,
    std::vector<std::pair<std::string, std::string>> *out) const {
  return mvcc_.Scan(start_key, end_key, limit, snapshot_ts, out);
}
```

`kv_state_machine.h` 已包含 `<optional>`，因此 `.cc` 不需要重复增加该 include。

---

## 四、修改 `src/service/kv_service.h`

### 1. 在 `namespace raftkv {` 后、Closure 类之前增加两个公共读结果填充函数

```cpp
// 根据 snapshot_ts 选择旧 data CF 或 MVCC 快照读，并填充 RPC 响应。
void FillGetResponse(const kv::GetRequest &request, kv::GetResponse *response,
                     KVStateMachine *fsm);

void FillScanResponse(const kv::ScanRequest &request,
                      kv::ScanResponse *response, KVStateMachine *fsm);
```

### 2. 用下面代码完整替换 `ReadIndexClosure::Run`

```cpp
  void Run() override {
    if (status().ok()) {
      FillGetResponse(*req_, resp_, fsm_);
    } else {
      resp_->set_success(false);
      resp_->set_error(status().error_str());
    }
    done_->Run();
    delete this;
  }
```

### 3. 用下面代码完整替换 `ScanReadIndexClosure::Run`

```cpp
  void Run() override {
    if (status().ok()) {
      FillScanResponse(*req_, resp_, fsm_);
    } else {
      resp_->set_success(false);
      resp_->set_error(status().error_str());
    }
    done_->Run();
    delete this;
  }
```

Closure 必须调用统一函数，不能继续直接调用旧 `fsm_->Get/Scan`，否则 Lease Read 已支持 MVCC、Log Read 却仍会读取 `data` CF。

---

## 五、修改 `src/service/kv_service.cc`

### 1. 在标准库 include 区增加

```cpp
#include <optional>
#include <vector>
```

### 2. 在文件顶部匿名 namespace 中保留 `HybridNowTs`，并在匿名 namespace 结束后增加以下完整代码

放置位置：现有 `} // namespace` 与 `RediretIfNotLeader` 之间。

```cpp
namespace {

template <typename ResponseType>
void FillMvccReadError(ResponseType *response, const MvccError &err) {
  response->set_success(false);
  response->set_error(err.message);

  if (err.kind == MvccError::kLocked) {
    auto *lock = response->mutable_lock_conflict();
    lock->set_key(err.key);
    lock->set_primary(err.primary);
    lock->set_lock_ts(err.lock_ts);
  }
}

} // namespace

void FillGetResponse(const kv::GetRequest &request,
                     kv::GetResponse *response, KVStateMachine *fsm) {
  response->clear_error();
  response->clear_value();
  response->clear_lock_conflict();
  response->set_found(false);

  if (request.snapshot_ts() == 0) {
    std::string value;
    bool found = fsm->Get(request.key(), &value);
    response->set_success(true);
    response->set_found(found);
    if (found) {
      response->set_value(value);
    }
    return;
  }

  std::optional<std::string> value;
  MvccError err = fsm->TxnGet(request.key(), request.snapshot_ts(), &value);
  if (!err.ok()) {
    FillMvccReadError(response, err);
    return;
  }

  response->set_success(true);
  response->set_found(value.has_value());
  if (value) {
    response->set_value(*value);
  }
}

void FillScanResponse(const kv::ScanRequest &request,
                      kv::ScanResponse *response, KVStateMachine *fsm) {
  response->clear_error();
  response->clear_kvs();
  response->clear_lock_conflict();

  if (request.snapshot_ts() == 0) {
    auto kvs =
        fsm->Scan(request.start_key(), request.end_key(), request.limit());
    response->set_success(true);
    for (const auto &[key, value] : kvs) {
      auto *pair = response->add_kvs();
      pair->set_key(key);
      pair->set_value(value);
    }
    return;
  }

  std::vector<std::pair<std::string, std::string>> kvs;
  MvccError err =
      fsm->TxnScan(request.start_key(), request.end_key(), request.limit(),
                   request.snapshot_ts(), &kvs);
  if (!err.ok()) {
    FillMvccReadError(response, err);
    return;
  }

  response->set_success(true);
  for (const auto &[key, value] : kvs) {
    auto *pair = response->add_kvs();
    pair->set_key(key);
    pair->set_value(value);
  }
}
```

注意：文件原来已经有一个包含 `HybridNowTs` 的匿名 namespace。上面再开一个匿名 namespace 在 C++ 中合法；也可以把 `FillMvccReadError` 合并进原匿名 namespace。

### 3. 用下面代码完整替换 `KVServiceImpl::Get`

```cpp
// ── Get RPC：snapshot_ts > 0 自动按线性一致读处理 ────────────────────
void KVServiceImpl::Get(::google::protobuf::RpcController * /*controller*/,
                        const kv::GetRequest *request,
                        kv::GetResponse *response,
                        ::google::protobuf::Closure *done) {
  // 当前没有 follower safe-ts。即使调用方没有设置 linearizable，
  // MVCC 快照读也必须在 leader 上经过 applied barrier。
  const bool need_barrier =
      request->linearizable() || request->snapshot_ts() > 0;

  if (!need_barrier) {
    FillGetResponse(*request, response, fsm_);
    done->Run();
    return;
  }

  if (RediretIfNotLeader(response, done)) {
    return;
  }

  if (node_->is_leader_lease_valid()) {
    braft::NodeStatus status;
    node_->get_status(&status);
    const int64_t target = status.committed_index;

    if (fsm_->WaitApplied(target, /*timeout_ms=*/50)) {
      FillGetResponse(*request, response, fsm_);
      done->Run();
      return;
    }

    // 不能在 WaitApplied 超时后继续直接读，否则不再满足线性一致性。
    LOG(WARNING) << "Lease Read: applied index 未追上 committed=" << target
                 << " applied=" << fsm_->LastAppliedIndex()
                 << "，回退到 Log Read";
  }

  // 租约无效或 applied barrier 超时：追加一个只建立顺序点的 OP_GET 日志。
  // 实际读取由 ReadIndexClosure::Run 调用 FillGetResponse 完成。
  kv::KvOperation op;
  op.set_op(kv::OP_GET);
  op.set_key(request->key());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }

  braft::Task task;
  task.data = &log;
  task.done = new ReadIndexClosure(request, response, done, fsm_);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}
```

### 4. 用下面代码完整替换 `KVServiceImpl::Scan`

```cpp
// ── Scan RPC：snapshot_ts > 0 自动按线性一致读处理 ───────────────────
void KVServiceImpl::Scan(::google::protobuf::RpcController * /*controller*/,
                         const kv::ScanRequest *request,
                         kv::ScanResponse *response,
                         ::google::protobuf::Closure *done) {
  const bool need_barrier =
      request->linearizable() || request->snapshot_ts() > 0;

  // 保持原 Scan 行为：无论强读还是弱读，都只由 leader 提供服务。
  if (RediretIfNotLeader(response, done)) {
    return;
  }

  if (!need_barrier) {
    // snapshot_ts == 0 的旧弱读路径，仍读取 data CF。
    FillScanResponse(*request, response, fsm_);
    done->Run();
    return;
  }

  if (node_->is_leader_lease_valid()) {
    braft::NodeStatus status;
    node_->get_status(&status);
    const int64_t target = status.committed_index;

    if (fsm_->WaitApplied(target, /*timeout_ms=*/50)) {
      FillScanResponse(*request, response, fsm_);
      done->Run();
      return;
    }

    LOG(WARNING) << "Lease Scan: applied index 未追上 committed=" << target
                 << " applied=" << fsm_->LastAppliedIndex()
                 << "，回退到 Log Read";
  }

  // 租约无效或 applied barrier 超时：通过 OP_SCAN 建立 Raft 顺序点。
  kv::KvOperation op;
  op.set_op(kv::OP_SCAN);
  op.set_key(request->start_key());
  op.set_value(request->end_key());

  butil::IOBuf log;
  butil::IOBufAsZeroCopyOutputStream stream(&log);
  if (!op.SerializeToZeroCopyStream(&stream)) {
    response->set_success(false);
    response->set_error("序列化失败");
    done->Run();
    return;
  }

  braft::Task task;
  task.data = &log;
  task.done = new ScanReadIndexClosure(request, response, done, fsm_);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}
```

这个版本保持现有 Scan 的 leader-only 行为；Get 的旧弱读仍可由 follower 直接读取。两者的共同规则是：`snapshot_ts > 0` 必须经过 leader 和 applied barrier。

---

## 六、完整调用关系

### 非事务读取

```text
Get/Scan(snapshot_ts = 0)
  -> FillGetResponse/FillScanResponse
  -> KVStateMachine::Get/Scan
  -> RocksDbStorage::Get/Scan
  -> data CF
```

### 事务快照读取

```text
Get/Scan(snapshot_ts > 0)
  -> 强制 leader
  -> leader lease + WaitApplied
     或 OP_GET/OP_SCAN Log Read barrier
  -> FillGetResponse/FillScanResponse
  -> KVStateMachine::TxnGet/TxnScan
  -> MvccTxn::Get/Scan
  -> lock/write/default CF
```

### 遇到锁

```text
MvccTxn::Get/Scan
  -> MvccError::kLocked
  -> response.success = false
  -> response.lock_conflict = {key, primary, lock_ts}
  -> 客户端调用 CheckTxnStatus(primary, lock_ts)
  -> 根据 committed / remaining_ttl_ms 决定重试、等待或清理 secondary
```

---

## 七、proto 重新生成和编译

项目的 CMake 已负责 protobuf 生成时，执行：

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

如果当前构建目录已经配置完成，可以只执行：

```bash
cmake --build build -j"$(nproc)"
```

不要手工修改生成的 `kv.pb.h/kv.pb.cc`。

---

## 八、必须验证的用例

1. **旧 Get 回归**
   - 普通 Put 写入 `data` CF。
   - `snapshot_ts=0` 能读到。
   - `snapshot_ts>0` 不应从 `data` CF 回退读取。

2. **事务提交后快照可见**
   - `Prewrite(start_ts=100)`。
   - `Commit(commit_ts=120)`。
   - Get `snapshot_ts=119` 不可见。
   - Get `snapshot_ts=120` 可见。

3. **锁结构化返回**
   - `Prewrite(start_ts=100)` 后不提交。
   - Get `snapshot_ts=100`。
   - `success=false`。
   - `lock_conflict.key/primary/lock_ts` 均正确。

4. **更晚锁不阻塞旧快照**
   - 已提交旧版本 `commit_ts=80`。
   - 另一个事务留下 `start_ts=120` 的锁。
   - Get `snapshot_ts=100` 应返回旧版本，不返回锁错误。

5. **Scan 锁错误**
   - 扫描区间内放置一个未提交锁。
   - Scan 返回第一个锁的结构化信息，不返回部分结果。

6. **follower redirect**
   - 向 follower 发送 `snapshot_ts>0, linearizable=false` 的 Get/Scan。
   - 因代码自动提升为强读，应返回 leader redirect。

7. **applied barrier 超时**
   - 人为让 applied index 落后。
   - Lease 分支超时后必须回退 Log Read，不能继续直接读状态机。

---

## 九、与本次修改相关但需要单独处理的问题

当前 `KVStateMachine::on_apply` 将旧 Put/Delete 暂存在两个 vector，循环结束后才 `BatchWrite`。但每条日志的 `AsyncClosureGuard` 会在该次循环结束时释放，因此可能先向客户端返回成功，再真正写 RocksDB；同时“先执行全部 Put、再执行全部 Delete”会改变同一批 Raft 日志的原始顺序。

这不会阻止上述代码编译，但会影响旧路径的 Log Read barrier 和线性一致性。建议后续单独修改为：

- 每条 OP_PUT/OP_DELETE 在 closure 返回前立即落盘；或
- 使用保持日志原顺序的单个 WriteBatch，并将相关 closure 延迟到 WriteBatch 成功之后。

此外，`MvccTxn::Prewrite/Commit/Rollback` 当前没有检查 `storage_->Write(...)` 的返回状态，也应单独补上存储错误传播。
