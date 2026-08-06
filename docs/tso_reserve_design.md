# LocalTso 预留式发号：设计与修改记录

## 1. 背景与问题

事务 commit_ts 的分配点在 `KVServiceImpl::TxnCommit`（`commit_ts == 0` 时由 leader
分配后写进 Raft 日志，各副本确定性一致）。原实现直接用裸墙钟：

```cpp
uint64_t ts = HybridNowTs();   // gettimeofday_ms() << 18
req.set_commit_ts(ts > req.start_ts() ? ts : req.start_ts() + 1);
```

两个问题：

1. **不经过 LocalTso**：`local_tso.h` 的单调性保证（CAS 循环、抗时钟回拨）没有用上；
2. **failover 可能重复/回退发号**：新 leader 的墙钟若偏慢，可能发出比旧 leader
   更小的时间戳，破坏 MVCC 时间戳全序。

正确协议是**预留式发号（先持久化、后发号）**：

```text
持久化承诺（reserve 上界，走 Raft 日志）→ 只允许发号到该上界 → 快用完再预留下一批
恢复 / leader 切换：RecoverTo(已持久化上界)
```

任何崩溃点都不会重复发号：旧 leader 发过的任何号都严格小于已持久化的上界，
新 leader 从上界起跳。反例（错误做法）是"先发号、每 N 个才回头记一条日志"——
崩溃瞬间最后一批已发出但未入账，恢复后会重复发号。

⚠️ 另一个必须遵守的点：新 leader **不能从 now() 重启发号**（墙钟可能偏慢），
起点必须来自持久化状态；物理时钟只作为 `max(phys, last+1)` 里的参考下界。

## 2. 修改总览（6 个文件）

| 文件 | 修改 |
|---|---|
| `proto/kv.proto` | `TsoResponse` 加 `success/error`；新增 `OP_TSO_RESERVE = 9` |
| `src/raft/local_tso.h` | 新增 `NextBatch(count)` 批量取号 |
| `src/raft/kv_state_machine.h` | 新增 `LocalTso tso_`、`tso_reserved_`、`Tso()/TsoReserved()`、`kTsoReserveBatch` |
| `src/raft/kv_state_machine.cc` | apply `OP_TSO_RESERVE`；commit 时 `RecoverTo(commit_ts)`；`on_leader_start` 时 `RecoverTo(上界)`；meta CF 持久化与恢复 |
| `src/service/kv_service.h` | 新增 `GetTso` 声明、`WithTsoReserve` 声明、`TsoReserveClosure` |
| `src/service/kv_service.cc` | 实现 `GetTso` 与 `WithTsoReserve`；`TxnCommit` 的 commit_ts 分配改走 LocalTso |
| `tests/integration/txn_probe.cc` | 新增 `--op=tso --count=N` 子命令 |

## 3. proto 变更

```protobuf
enum OperationType {
    ...
    OP_TXN_RESOLVE =8;
    OP_TSO_RESERVE =9; // TSO 预留上界：先持久化「未来上界」再发号，failover 不重复发号
}

message TsoRequest {
  uint32 count = 1;         // 批量取号，默认 1
}

message TsoResponse {
  uint64 ts = 1;            // 返回[ts, ts+count)区间
  string redirect = 2;
  bool success = 3;         // 新增：与其他 RPC 保持一致
  string error = 4;         // 新增
}
```

`OP_TSO_RESERVE` 日志复用 `KvOperation.key` 携带新上界（十进制字符串），
与 `OP_TXN_RESOLVE` 携带 now_ts 的方式一致。

## 4. local_tso.h：批量取号

```cpp
// 批量取号：一次 CAS 原子跳 count，返回区间起点，区间为 [start, start+count)。
// next >= last + count 保证 start = next - count + 1 > last，与已发号不重叠。
uint64_t NextBatch(uint32_t count) {
  uint64_t phys = NowMs() << kLogicalBits;
  uint64_t last = last_.load(std::memory_order_relaxed);
  uint64_t next;
  do {
    next = std::max(phys, last + count);
  } while (
      !last_.compare_exchange_weak(last, next, std::memory_order_acq_rel));
  return next - count + 1;
}
```

与 `Next()` 同一个 CAS 模式，只是一次跳 count 格；
`next >= last + count` ⇒ `start > last`，与已发号区间不重叠。

## 5. 状态机：预留上界的复制、持久化与恢复

`kv_state_machine.h` 新增：

```cpp
LocalTso &Tso() { return tso_; }
uint64_t TsoReserved() const;                       // 已持久化的预留上界
static constexpr uint64_t kTsoReserveBatch = 1ULL << 16;  // 每批预留号段大小
// private:
LocalTso tso_;
std::atomic<uint64_t> tso_reserved_{0};
void LoadTsoReserved();                             // 从 meta CF 读回上界
```

`kv_state_machine.cc` 四处关键逻辑：

```cpp
// (1) on_apply：应用预留日志（各副本一致），并随 WriteBatch 落 meta CF
case kv::OP_TSO_RESERVE: {
  const uint64_t reserve = strtoull(operation.key().c_str(), nullptr, 10);
  uint64_t cur = tso_reserved_.load(std::memory_order_relaxed);
  if (reserve > cur) {
    tso_reserved_.store(reserve, std::memory_order_release);
    batch.Put("meta", kTsoReservedKey, operation.key());   // "__tso_reserved__"
  }
  break;
}

// (2) on_apply OP_TXN_COMMIT：follower 的 TSO 跟随日志中的 commit_ts，failover 后不回退
tso_.RecoverTo(req.commit_ts());

// (3) on_leader_start：新 leader 直接跳过整段预留，绝不重复发号
//     （⚠️ 不能从 now() 重启：新 leader 墙钟可能偏慢）
tso_.RecoverTo(tso_reserved_.load(std::memory_order_acquire));

// (4) 构造函数与 on_snapshot_load 之后：LoadTsoReserved() 从 meta CF 读回上界
```

为什么 apply 时只更新 `tso_reserved_` 而不 `RecoverTo(reserve)`：
leader 也会 apply 自己的预留日志，若此时把 `last_` 直接跳到上界，
整段号就被浪费并立刻触发下一次预留，形成日志风暴。
`RecoverTo(上界)` 只在 **leader 切换** 时执行——那时旧 leader 可能已把号发到上界，
必须整段跳过。

## 6. 服务层：WithTsoReserve 与 GetTso

核心工具（`kv_service.cc`）：

```cpp
// 预留式发号（先持久化、后发号）：上界不足时提交 OP_TSO_RESERVE 日志，
// apply 成功后才回调 cont 继续发号。
void KVServiceImpl::WithTsoReserve(
    uint32_t count, std::function<void(const butil::Status &)> cont) {
  const uint64_t upto = fsm_->Tso().Current() + count;
  if (upto <= fsm_->TsoReserved()) {
    cont(butil::Status::OK());          // 热路径：零 Raft 写
    return;
  }
  // 新上界覆盖本次需求 + 一整段预留，避免每次发号都写日志
  const uint64_t new_reserve =
      std::max(upto, HybridNowTs() + count) + KVStateMachine::kTsoReserveBatch;

  kv::KvOperation op;
  op.set_op(kv::OP_TSO_RESERVE);
  op.set_key(std::to_string(new_reserve));
  // ...序列化后 node_->apply(task)，task.done = new TsoReserveClosure(cont)
}
```

`GetTso` RPC：

```cpp
void KVServiceImpl::GetTso(..., const kv::TsoRequest *request,
                           kv::TsoResponse *response, ...) {
  if (RediretIfNotLeader(response, done)) return;   // 只有 leader 能发号
  uint32_t count = request->count() == 0 ? 1 : request->count();
  if (count > kMaxTsoBatch) count = kMaxTsoBatch;   // 上限 2^18，防逻辑位借位失控
  WithTsoReserve(count, [this, count, response, done](const butil::Status &s) {
    if (!s.ok()) { response->set_success(false); response->set_error(...); done->Run(); return; }
    response->set_success(true);
    response->set_ts(fsm_->Tso().NextBatch(count)); // 区间 [ts, ts+count)
    done->Run();
  });
}
```

`TsoReserveClosure`（`kv_service.h`）：持有 `std::function` 回调，
apply 成功/失败后带 `butil::Status` 回调，自删。

## 7. TxnCommit 的 commit_ts 分配改走 LocalTso

```cpp
// 修改前：uint64_t ts = HybridNowTs();
// 修改后：
kv::TxnCommitRequest req = *request;
if (req.commit_ts() == 0) {
  WithTsoReserve(1, [this, req = std::move(req), response, done](
                        const butil::Status &s) mutable {
    if (!s.ok()) { /* set_success(false) + set_error 后应答 */ return; }
    uint64_t ts = fsm_->Tso().Next();
    req.set_commit_ts(ts > req.start_ts() ? ts : req.start_ts() + 1);
    ApplyTxnOp(kv::OP_TXN_COMMIT, req.SerializeAsString(), response, done);
  });
  return;
}
```

`CheckTxnStatus` 的 now_ts **保留** `HybridNowTs()`：它是 TTL 判定的物理时间语义
（`(now_ts>>18) - (lock_ts>>18)` = 经过的毫秒数），不是发号，无需单调性保证。

## 8. txn_probe 新增 tso 子命令

```bash
./txn_probe --addr=127.0.0.1:8200 --op=tso --count=10
# 输出：success=0|1 ts=... count=... redirect=... error=...
```

与其他子命令一致：直连 --addr、max_retry=0、不跟随 redirect，
RPC 层失败输出 `rpc_error=` 且退出码 2。

## 9. 正确性论证（面试可讲）

- **不变式**：任何已发出的 ts ≤ 已通过 Raft 提交的预留上界。
  发号前 `WithTsoReserve` 检查 `Current() + count ≤ TsoReserved()`，
  不足则先提交预留日志、apply 后才继续。
- **failover 不重复**：新 leader `on_leader_start` 时 `RecoverTo(上界)`，
  从上界起跳；旧 leader 发过的任何号都 < 上界。
- **failover 不回退**：起点来自持久化状态而非 now()；此外 apply commit 日志时
  follower 也 `RecoverTo(commit_ts)` 兜底。
- **快照/日志截断后可恢复**：上界随 apply 写入 meta CF（`__tso_reserved__`），
  checkpoint 快照自带；启动与 on_snapshot_load 后 `LoadTsoReserved()` 读回。
- **热路径开销**：预留段内发号是一次原子 CAS，零 Raft 写；
  每 2^16 个号才写一条预留日志。

## 10. 已知取舍

1. 预留日志的 RPC 应答（closure）发生在 meta CF 落盘之前，但正确性由
   Raft 日志本身保证（崩溃后重放会重新 apply 预留），meta CF 只服务于
   快照截断后的恢复。
2. `WithTsoReserve` 的检查与实际 `Next()` 之间存在极小的并发窗口
   （多个请求同时通过检查后合计超出上界）。学习项目可接受；
   严格实现可在 `NextBatch` 里带上界参数做 CAS 内校验，超界即失败重试。
3. 客户端传入非本集群分配的超大 `start_ts` 时，`max(ts, start_ts+1)` 的兜底
   可能突破预留上界。生产实现应拒绝非法 start_ts。

## 11. 验证步骤

```bash
cd build && cmake .. && make -j4          # protoc 重新生成 + 全量编译
./txn_integration_test                     # 6/6
bash ../tests/integration/txn_failover_test.sh   # ALL PASS

# TSO 专项
./txn_probe --addr=<leader>   --op=tso --count=5   # success=1 ts=非0
./txn_probe --addr=<leader>   --op=tso --count=5   # 第二次 ts ≥ 第一次 + 5
./txn_probe --addr=<follower> --op=tso             # success=0 redirect=<leader>
# kill leader 后在新 leader 再取一次，ts 应大于切换前取到的所有 ts
```

## 12. 修订：发号收紧到预留上界内（NextBatchBounded）

第 10 节的取舍 2 实际上不止是并发窗口问题：`Next()/NextBatch()` 会取
`max(phys, last+n)`，而 `kTsoReserveBatch = 2^16` 只相当于 0.25ms 物理时间，
墙钟推进本身就会让发出的 ts 冲过已持久化上界——failover 后新 leader
`RecoverTo(上界)` 起跳，仍可能与旧 leader 已发出的号重叠，破坏核心不变式。

修复（3 个文件）：

- `local_tso.h` 新增 `NextBatchBounded(count, bound, *start)`：
  上界校验放进 CAS 循环内，`next > bound` 直接返回 false，
  并发下不会发出任何超出已持久化上界的号。
- `kv_service.{h,cc}`：`WithTsoReserve` 重构为
  `AllocateTso(count, attempt, cont(status, start))`——先尝试
  `NextBatchBounded(count, TsoReserved())`，失败则提交 OP_TSO_RESERVE
  （新上界 = `max(Current(), HybridNowTs()) + count + kTsoReserveBatch`），
  apply 成功后重试，最多 4 次（防无限递归）。`GetTso` 与 `TxnCommit`
  的发号均改走 `AllocateTso`，直接使用回调给出的号段起点，
  不再在回调里二次调用 `Next()`。

修订后不变式严格成立：任何已发出的 ts ≤ 已通过 Raft 提交的预留上界。
取舍 3（客户端传入超大 start_ts 时 `start_ts+1` 兜底可能越界）仍保留。

验证同第 11 节；尚未在本机重新编译，改动后请先 `cmake --build build` 并
重跑 `txn_integration_test` / `txn_failover_test.sh` / `--op=tso` 专项。

### 12.1 修订：取舍 3 收紧（拒绝超前 start_ts）

`TxnCommit` 分配 commit_ts 的回调中，若 `AllocateTso` 给出的 ts ≤ start_ts，
说明 start_ts 并非本集群 TSO 发出（超前伪造），直接拒绝请求，
不再用 `start_ts+1` 兜底（兜底会突破预留上界）。合法流程不受影响：
本集群发出的 start_ts 必小于之后新分配的任何 ts。
