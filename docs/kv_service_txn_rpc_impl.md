# KvService 追加 4 个事务 RPC 实现方案

> 目标：为 `KVServiceImpl` 追加 `TxnPrewrite / TxnCommit / TxnRollback / CheckTxnStatus` 四个 RPC，
> 完全照抄现有 `Put` 的「打包日志 → node_->apply → 等 closure 回调」模式，含非 leader redirect。
> 本文档只给增量代码，不直接改源码。

---

## 0. 前置修正：proto 笔误

`kv.proto` 第 92 行有一个全角冒号，protoc 编译会直接报错，必须先删掉：

```diff
-  uint64 remaining_ttl_ms： =6;
+  uint64 remaining_ttl_ms = 6;
```

---

## 1. 总体设计

### 1.1 日志载荷：整个请求打包进 KvOperation.value

`KvOperation` 只有 `op / key / value` 三个字段，而事务请求携带 `mutations、start_ts、ttl` 等复杂结构。
做法：**把整个事务请求 protobuf 序列化后塞进 `value` 字段**，`op` 用来区分类型：

| RPC            | OperationType     | value 内容（序列化后）        |
|----------------|-------------------|-------------------------------|
| TxnPrewrite    | `OP_TXN_PREWRITE` | `TxnPrewriteRequest`          |
| TxnCommit      | `OP_TXN_COMMIT`   | `TxnCommitRequest`            |
| TxnRollback    | `OP_TXN_ROLLBACK` | `TxnRollbackRequest`          |
| CheckTxnStatus | `OP_TXN_RESOLVE`  | `CheckTxnStatusRequest`       |

`on_apply` 时按 `op` 反序列化出对应请求，再执行确定性的状态机逻辑。

### 1.2 结果回传：闭包暴露 response，由 on_apply 填结果

现有 `KVClosure` 只会 set_success(true/false)，但事务 RPC 需要回传业务结果
（prewrite 冲突信息、实际 commit_ts、CheckTxnStatus 的 committed/remaining_ttl_ms）。

做法：新增泛型 `TxnClosure`，暴露 `response()`；leader 在 `on_apply` 里通过
`iter.done()` 拿到闭包，直接把执行结果写进 response。closure 的 `Run()` 只兜底
处理 raft 层失败（丢 leader、超时）。follower 上 `iter.done() == nullptr`，只改状态机不填响应。

### 1.3 关键：commit_ts 必须在提交日志「之前」分配

`TxnCommitRequest.commit_ts == 0` 表示由 leader 分配。**不能在 on_apply 时再取
LocalTso**——那样三个副本 apply 出的 commit_ts 会不一致，状态机发散。
正确做法：leader 在 RPC handler 里（打包日志前）就从 LocalTso 取号，把 commit_ts
**改写进日志载荷**，保证所有副本 apply 的是同一个 ts。

同理 `CheckTxnStatus` 判定 TTL 是否过期，也要在 handler 里用 leader 本地时钟判定，
把「判定结果/动作」写进日志，而不是让每个副本各自看表。
（最简单的做法：handler 里先读状态机拿到锁信息判定，把要执行的动作编码进日志；
入门版可以先接受 handler 判定 + apply 执行的轻微竞态，后续再收紧。）

---

## 2. kv_service.h 增量

```cpp
// ── 事务 RPC 泛型 Closure ────────────────────────────────────────────
// 与 KVClosure 的区别：暴露 response()，成功路径的业务结果
// （conflict_*, commit_ts, committed 等）由 on_apply 直接写入 response；
// Run() 只兜底 raft 层失败（丢 leader / 超时 / term 变更）。
template <typename ResponseType> class TxnClosure : public braft::Closure {
public:
  TxnClosure(ResponseType *response, google::protobuf::Closure *done)
      : response_(response), done_(done) {}

  ResponseType *response() { return response_; }

  void Run() override {
    if (!status().ok()) {
      response_->set_success(false);
      response_->set_error(status().error_str());
      LOG(WARNING) << "事务操作 raft 失败: " << status().error_cstr();
    }
    // status().ok() 时，success 与结果字段已由 on_apply 填好，这里不再覆盖
    done_->Run();
    delete this;
  }

private:
  ResponseType *response_;
  google::protobuf::Closure *done_;
};
```

`KVServiceImpl` 类里追加 4 个 override 声明（放在 `Scan` 声明之后）：

```cpp
  // ── MVCC 事务 RPC ──
  void TxnPrewrite(::google::protobuf::RpcController *controller,
                   const kv::TxnPrewriteRequest *request,
                   kv::TxnPrewriteResponse *response,
                   ::google::protobuf::Closure *done) override;

  void TxnCommit(::google::protobuf::RpcController *controller,
                 const kv::TxnCommitRequest *request,
                 kv::TxnCommitResponse *response,
                 ::google::protobuf::Closure *done) override;

  void TxnRollback(::google::protobuf::RpcController *controller,
                   const kv::TxnRollbackRequest *request,
                   kv::TxnRollbackResponse *response,
                   ::google::protobuf::Closure *done) override;

  void CheckTxnStatus(::google::protobuf::RpcController *controller,
                      const kv::CheckTxnStatusRequest *request,
                      kv::CheckTxnStatusResponse *response,
                      ::google::protobuf::Closure *done) override;
```

> 四个 Response 都有 `success / redirect / error` 字段，现有的
> `RediretIfNotLeader` 模板可直接复用，零改动。

---

## 3. kv_service.cc 增量

先加一个私有辅助函数，抽掉四个 handler 的公共部分（打包日志 → apply）：

```cpp
// ── 内部工具：把事务请求打包成 KvOperation 日志并提交 raft ──────────
// req_payload：已序列化的事务请求；closure 所有权移交给 braft
template <typename ResponseType>
void KVServiceImpl::ApplyTxnOp(kv::OperationType op_type,
                               std::string req_payload,
                               ResponseType *response,
                               google::protobuf::Closure *done) {
  kv::KvOperation op;
  op.set_op(op_type);
  op.set_value(std::move(req_payload));

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
  task.done = new TxnClosure<ResponseType>(response, done);
  task.expected_term = fsm_->LeaderTerm();
  node_->apply(task);
}
```

（头文件私有区加对应模板声明：）

```cpp
  template <typename ResponseType>
  void ApplyTxnOp(kv::OperationType op_type, std::string req_payload,
                  ResponseType *response, google::protobuf::Closure *done);
```

### 3.1 TxnPrewrite

```cpp
// ── TxnPrewrite RPC：2PC 第一阶段，需经过 Raft 共识 ──────────────────
void KVServiceImpl::TxnPrewrite(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::TxnPrewriteRequest *request, kv::TxnPrewriteResponse *response,
    ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  if (request->mutations().empty() || request->primary().empty() ||
      request->start_ts() == 0) {
    response->set_success(false);
    response->set_error("非法 prewrite 请求：mutations/primary/start_ts 缺失");
    done->Run();
    return;
  }
  ApplyTxnOp(kv::OP_TXN_PREWRITE, request->SerializeAsString(), response,
             done);
}
```

### 3.2 TxnCommit（commit_ts 在 handler 分配）

```cpp
// ── TxnCommit RPC：2PC 第二阶段，需经过 Raft 共识 ────────────────────
void KVServiceImpl::TxnCommit(::google::protobuf::RpcController * /*controller*/,
                              const kv::TxnCommitRequest *request,
                              kv::TxnCommitResponse *response,
                              ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  // 【关键】commit_ts=0 时由 leader 在此处分配，并改写进日志载荷。
  // 不能推迟到 on_apply 再取号——各副本会分配到不同 ts，状态机发散。
  kv::TxnCommitRequest req = *request;
  if (req.commit_ts() == 0) {
    req.set_commit_ts(fsm_->Tso()->Allocate(1)); // LocalTso 取一个号
  }
  if (req.commit_ts() <= req.start_ts()) {
    response->set_success(false);
    response->set_error("commit_ts 必须大于 start_ts");
    done->Run();
    return;
  }
  ApplyTxnOp(kv::OP_TXN_COMMIT, req.SerializeAsString(), response, done);
}
```

### 3.3 TxnRollback

```cpp
// ── TxnRollback RPC：清锁 + 写回滚标记，需经过 Raft 共识 ─────────────
void KVServiceImpl::TxnRollback(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::TxnRollbackRequest *request, kv::TxnRollbackResponse *response,
    ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  ApplyTxnOp(kv::OP_TXN_ROLLBACK, request->SerializeAsString(), response,
             done);
}
```

### 3.4 CheckTxnStatus

```cpp
// ── CheckTxnStatus RPC：查/清 primary 残锁，可能改状态机，走 Raft ────
void KVServiceImpl::CheckTxnStatus(
    ::google::protobuf::RpcController * /*controller*/,
    const kv::CheckTxnStatusRequest *request,
    kv::CheckTxnStatusResponse *response, ::google::protobuf::Closure *done) {
  if (RediretIfNotLeader(response, done)) {
    return;
  }
  ApplyTxnOp(kv::OP_TXN_RESOLVE, request->SerializeAsString(), response,
             done);
}
```

---

## 4. 状态机侧需要配合的改动（on_apply）

`KVStateMachine::on_apply` 的 switch 里追加 4 个 case，套路一致：
反序列化请求 → 执行确定性事务逻辑 → 若是 leader（`iter.done() != nullptr`）把结果写回 response。

以 Prewrite 为例：

```cpp
case kv::OP_TXN_PREWRITE: {
  kv::TxnPrewriteRequest req;
  req.ParseFromString(op.value());
  PrewriteResult r = mvcc_.Prewrite(req); // 锁检查 + 写 lock CF，确定性
  if (iter.done()) { // 只有 leader 持有闭包
    auto *c =
        dynamic_cast<TxnClosure<kv::TxnPrewriteResponse> *>(iter.done());
    auto *resp = c->response();
    resp->set_success(r.ok);
    if (!r.ok) {
      resp->set_error(r.error);
      resp->set_conflict_key(r.conflict_key);
      resp->set_conflict_primary(r.conflict_primary);
      resp->set_conflict_start_ts(r.conflict_start_ts);
      resp->set_conflict_commit_ts(r.conflict_commit_ts);
    }
  }
  break;
}
```

Commit / Rollback / Resolve 同理：

- `OP_TXN_COMMIT`：apply `mvcc_.Commit(keys, start_ts, commit_ts)`，
  leader 回填 `resp->set_commit_ts(req.commit_ts())`（handler 已确保非 0）。
- `OP_TXN_ROLLBACK`：apply `mvcc_.Rollback(keys, start_ts)`。
- `OP_TXN_RESOLVE`：apply `mvcc_.CheckTxnStatus(primary, lock_ts)`，
  leader 回填 `committed / commit_ts / remaining_ttl_ms`。

注意两点：

1. **on_apply 里不要做任何非确定性判断**（取时间戳、看本地时钟）。
   需要时间的决策（commit_ts 分配、TTL 判定）都放在 leader 的 RPC handler，
   把结论编码进日志。
2. 事务逻辑失败（撞锁、写冲突）**不算 raft 失败**：日志已经复制成功，
   `status().ok()` 为 true，业务失败信息由 on_apply 写进 response。
   这也是用 `TxnClosure` 而不是复用 `KVClosure` 的原因——
   `KVClosure` 会在 `status().ok()` 时无条件 `set_success(true)`，把业务失败盖掉。

---

## 5. 与现有 Put 模式的对照小结

| 步骤              | Put                          | 事务 RPC                                   |
|-------------------|------------------------------|--------------------------------------------|
| leader 检查       | `RediretIfNotLeader`         | 相同，直接复用                              |
| 日志载荷          | key/value 直接放进 KvOperation | 整个请求序列化进 `value`                  |
| closure           | `KVClosure`（只 set_success）| `TxnClosure`（暴露 response，on_apply 填结果）|
| expected_term     | `fsm_->LeaderTerm()`         | 相同                                        |
| 结果来源          | raft status                  | raft status + on_apply 的业务结果           |
