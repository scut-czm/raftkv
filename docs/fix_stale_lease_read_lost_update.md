# 银行转账混沌测试总额不守恒：根因与修复

## 现象

`tests/chaos/txn_bank_chaos_test.sh 60`：

```
审计通过: 3   审计重试: 0   审计违规: 22
最终总额: 16005（期望 16000）
lock CF 为空 / write CF 无悬空 start_ts  → 全部 PASS
```

违规成簇出现在每次 `kill -9` + 重启之后；总额向两个方向偏移
（15992 / 16006），且**压测停止、残锁清理完毕后仍然是 16005**——
说明不是暂态可用性问题，而是已提交状态被真实破坏。

## 定位推理

1. 存储层终态是自洽的（无残锁、无悬空 write 记录、`start_ts < commit_ts`），
   所以不是 Prewrite/Commit 的 WriteBatch 原子性问题；
2. 2PC 的 Prewrite / Commit 各自是**一条** Raft 日志（`OP_TXN_PREWRITE` /
   `OP_TXN_COMMIT` 携带全部 key），apply 时串行执行，因此「部分提交」
   几乎不可能是主因；
3. 总额既能变多也能变少，且不自愈 → 典型的**丢失更新**（两个事务基于
   同一份旧数据各写一次，后者覆盖前者）；
4. 丢失更新在快照隔离下只可能来自「读到了旧快照，而 Prewrite 的写冲突
   检查又认为合法」。

## 根因：新 leader 尚未追平日志就服务 lease read

`KVServiceImpl::Get/Scan` 的强读路径原本是：

```cpp
if (node_->is_leader_lease_valid()) {
  node_->get_status(&status);
  if (fsm_->WaitApplied(status.committed_index, 50)) { 直接读状态机; }
}
```

`node_->is_leader()` / `is_leader_lease_valid()` 在**选举一结束**就为真，
而此时状态机的 applied index 可能远远落后。本项目的部署方式把这个窗口
放大到了极致：`server_main.cc` 因为关闭了 RocksDB WAL，**每次启动都会
删掉残留 DB，从 braft 快照 + 日志重建**。于是刚被 `kill -9` 拉起的节点：

- Raft 日志完整（页缓存未丢），可以正常参选甚至当选 leader；
- 状态机却是**空库**，正在从头重放上万条日志；
- 重启后 `get_status().committed_index` 也从低位开始恢复，
  于是 `WaitApplied(committed_index, 50ms)` **轻易「通过」**。

结果：`start_ts = T` 的事务从这个 leader 读到了**远早于 T 的快照**
（极端情况是空库）。而 Prewrite 的写冲突检查是
`SeekWrite(key).commit_ts >= start_ts`——被覆盖的那次提交
`commit_ts < T`，所以检查**合法通过**，旧值原地写回：

```
txn A: start_ts=100  读 acct_3=1000（真实最新是 commit_ts=90 的 995）
txn A: Prewrite acct_3 → 最新 commit_ts=90 < 100 → 无冲突 → 提交 1000
→ 995 那次转账的扣款被凭空抹掉，总额 +5，永久不自愈
```

这正是审计看到的「+5 / -8 且不恢复」。

## 修复

`fsm_->IsLeader()` 由 `on_leader_start` 置位，而 `on_leader_start`
是**按 apply 顺序**投递的回调：它为真时，状态机必然已经应用完前任
leader 提交的全部日志。因此把 lease read 的闸门收紧为：

```cpp
if (fsm_->IsLeader() && node_->is_leader_lease_valid()) { ... }
```

不满足时自动落到原有的 **Log Read** 路径：追加一条 `OP_GET` / `OP_SCAN`
日志，读取在该日志 apply 时执行——日志顺序天然保证「此前所有已提交
写入都已可见」，正在重放的节点会自然地把读请求排到重放之后，
只是变慢，不会读旧。

另外在 `on_apply` 的 `OP_TXN_COMMIT` 分支加了原子性告警：primary
（`keys[0]`，客户端 `write_buffer_` 是 `std::map`，primary 恒为最小 key）
提交成功而某个 secondary 提交失败时打 `LOG(ERROR)`，便于后续混沌运行
中区分「丢失更新」与「部分提交」两类根因。

## 复验

```bash
cd build && make kv_server bank_chaos_test mvcc_db_check
bash ../tests/chaos/txn_bank_chaos_test.sh 60
```

期望：`审计违规: 0`、最终总额 == 16000；`审计重试` 允许上升
（重启窗口内读请求走 Log Read 排在重放之后，会超时重试）。

## 遗留可优化项

- 每次重启都清空 RocksDB → 重放全量日志，窗口期读被迫走 Log Read。
  正解是让状态机自身持久化 applied index 并开启 WAL（或按更短周期做
  braft snapshot），使重启后无需从头重放；
- `MvccTxn::Get` 里 `assert(found)`：release 构建下会被优化掉，
  write 记录与数据版本不一致时应返回 `kStorageError` 而不是静默。


---

# 第二轮：修完 lease read 仍然违规 → 根因是 raft_sync=false + kill -9

第二次运行（已带 lease read 修复）：`审计违规: 23，最终总额 16013`，
并且测试 2 在节点 8200 上报了 2 把残锁。两件事分别是：

## 2.1 残锁 FAIL 是测试自身的误报

节点 8200 在压测结束前 15s 才被拉起，`write CF 共 3064 条`，而 8201/8202
都是 4550 条 —— 它还在从头重放日志（`server_main` 启动会清空 RocksDB），
清锁遍历产生的 resolve 日志它根本还没 apply。此时做离线检查，必然把
「还没重放到」误判成「残锁泄漏」。

修复：`txn_bank_chaos_test.sh` 停集群前不再 `sleep 3`，改成轮询 braft
内建状态页 `http://127.0.0.1:$PORT/raft_stat`，等三副本
`known_applied_index` 相等且 `>= last_log_index`（最多等 120s）。

## 2.2 总额不守恒的真凶：日志没 fsync 就应答，kill -9 丢已提交日志

集群启动参数里有 `--raft_sync=false`。此时 braft **不 fsync** raft 日志，
日志尾部可能仍停留在用户态 append buffer（`raft_max_append_buffer_size=4MB`）
中就已经向 leader 应答。于是：

```
leader L + follower A 应答某条 commit 日志 → 多数派 → L 认为已提交 → 回客户端成功
kill -9 A            → A 丢掉尾部（含这条日志）
之后 kill -9 L       → 剩下 {A(已丢), B(从未收到)} 组成多数派
→ 这条「已提交」的日志凭空消失：转账只生效一半，总额永久偏移
```

方向可正可负（丢的是扣款侧还是入账侧、丢的是 commit 还是 rollback 记录），
与观察到的 ±5 / ±13 完全吻合。这不是事务实现的 bug，而是**测试把 Raft
的持久性前提关掉了**：`raft_sync=false` 只适用于「不注入进程崩溃」的
性能测试。

修复：`scripts/start_cluster.sh` 的 `--raft_sync` / `--snapshot_interval_s`
改为读环境变量（默认保持原值），混沌脚本里 `export RAFT_SYNC=true`，
`restart_node` 同步改成 `--raft_sync=true`。

## 复验

```bash
bash ../tests/chaos/txn_bank_chaos_test.sh 60   # 不需要重新编译，只改了脚本
```

期望：`审计违规: 0`、最终总额 == 16000、三副本 lock CF 为空。
如果开了 fsync 仍然违规，那才说明事务逻辑本身有洞，下一步就该在
`OP_TXN_COMMIT` 的部分提交告警和客户端 resolve 链路上抓日志。


## 第三轮：开 raft_sync 后账户初始化失败（TSO 预留窗口过小）

现象：`初始化重试: Commit 失败: TSO 预留重试次数耗尽`，随后 9 次重试全部
`写冲突: key=acct_0 被事务 lock_ts=... 占用`，`FAIL: 账户初始化失败`。

根因 1（真 bug，被 raft_sync=true 放大）：
`kTsoReserveBatch = 1<<16`，而 TSO 低 18 位是逻辑位，1ms = 1<<18 ——
一次预留只覆盖 **0.25ms 物理时间**。`AllocateTso` 提交 OP_TSO_RESERVE 后
重试时 `NextBatchBounded` 取 `max(phys, last+n)`，只要墙钟推进超过 0.25ms
就再次越过上界。开 fsync 后一条 Raft 日志要几十毫秒，于是每次重试都失败，
4 次尝试必然耗尽。修复：预留改为覆盖固定物理时间窗口
`kTsoReserveAhead = 3000ms << 18`（PD 也是预留 3s），重试上限 4 → 8。
上界抬高不破坏不变式：failover 时新 leader `RecoverTo(上界)` 只是跳过
最多 3s 的号段，仍严格「已发出的 ts <= 已持久化上界」。

根因 2（测试侧）：初始化 Commit 失败后残锁不回滚（结果未知时不能盲目
回滚），而 Prewrite 撞锁只返回 kLocked、不触发 resolve，所以后续 9 次
重试永远撞同一把锁。修复：初始化重试前等 lock TTL（3s）过期，并用一次
快照读走 `SnapshotGetWithResolve` 清掉残锁。

改动：`src/raft/kv_state_machine.h`、`src/service/kv_service.cc`、
`tests/chaos/bank_chaos_test.cc`。

复验（需重新编译）：

```bash
cd build && make kv_server bank_chaos_test mvcc_db_check
bash ../tests/chaos/txn_bank_chaos_test.sh 60
```
