# 事务端到端混沌验证：银行转账测试总结

> 相关文件：
> - `tests/chaos/bank_chaos_test.cc` —— 银行转账压测驱动（含在线审计）
> - `tests/chaos/mvcc_db_check.cc` —— 离线 MVCC CF 一致性检查器
> - `tests/chaos/txn_bank_chaos_test.sh` —— 编排脚本（集群 + 故障注入 + 检查）
>
> 运行方式：
> ```bash
> cd build && cmake .. && make kv_server bank_chaos_test mvcc_db_check
> bash ../tests/chaos/txn_bank_chaos_test.sh 60   # 参数为压测秒数，默认 60
> ```

## 1. 测试目标

在**真实三节点 Raft 集群 + 随机节点宕机/重启**的混沌环境下，验证 MVCC
乐观事务（Percolator 2PC + 本地 TSO）的四条核心保证：

| # | 不变量 | 验证方式 |
|---|--------|----------|
| 1 | **原子性**：转账事务的两笔改动要么全生效要么全不生效 | 任意快照下总额恒等于 S |
| 2 | **快照隔离**：任一 start_ts 快照看到的都是一致的世界 | 审计线程用新快照读全部账户 |
| 3 | **无残锁泄漏**：事务结束后 lock CF 不残留锁 | 停集群后离线扫 lock CF |
| 4 | **无悬空提交**：write CF 每条 kPut 记录指向的数据版本必须存在 | 离线交叉校验 write ↔ default CF |

银行转账是检验事务原子性的经典 workload：`总额守恒` 是一个对任何
并发交错、任何宕机时机都必须成立的全局不变量——只要有一个事务
「只转出未转入」或「只转入未转出」地部分生效，总额立刻偏离 S。

## 2. 测试拓扑与参与者

```
┌─────────────────────────────────────────────────────────┐
│  3 节点 Raft 集群 (127.0.0.1:8200/8201/8202)             │
│  ← 混沌线程：每 10s 随机 kill -9 一个节点，5s 后拉起      │
└─────────────────────────────────────────────────────────┘
        ▲                    ▲                    ▲
   32 转账线程          1 审计线程           清锁遍历（结束时）
   随机转账事务      每 200ms 快照求和     逐账户 Get resolve 残锁
```

- **账户模型**：16 个账户 `acct_0 .. acct_15`，每个初始 1000，总额 S = 16000。
  初始化本身就是一个事务（16 个 Put 一次 Prewrite/Commit），保证原子落库。
- **转账线程 × 32**：每轮随机选 from ≠ to、金额 1~10：
  ```
  Transaction txn(&client);          // GetTso 取 start_ts（快照点）
  fb = txn.Get(from); tb = txn.Get(to);
  if (fb < amount) 放弃;             // 只读事务，无需提交
  txn.Put(from, fb-amount); txn.Put(to, tb+amount);
  txn.Commit(&err);                  // 2PC：Prewrite → Commit
  ```
  Commit 失败（写冲突/切主/选举窗口）不重试同一事务——乐观模型下直接
  开新事务重跑，与生产用法一致。
- **审计线程 × 1**：每 200ms 开一个**新快照事务**读全部 16 账户求和。
  三种结果分开统计：
  - `sum == S` → audit_ok；
  - 某账户读失败（节点切换中）→ audit_retry，**不算违规**（混沌下的
    正常暂态，可用性问题而非一致性问题）；
  - `sum != S` → **audit_violation，立即判定失败**。
- **混沌线程（shell 后台）**：循环「sleep 10s → 随机 kill -9 一个节点 →
  sleep 5s → 拉起该节点（保留数据目录）」。一次只杀一个且先恢复再杀
  下一个，**始终保有 majority**——集群应持续可写（只在选举窗口内短暂
  不可用），这是「验证正确性」而非「验证不可用时的行为」的前提。

## 3. 为什么总额守恒能被破坏（若实现有 bug）

审计读的是快照，所以它能抓到的都是**已提交状态的不一致**，典型根因：

1. **2PC 部分提交**：primary 提交了、secondary 没提交，且读者未帮
   提交就直接读到旧值 → 一笔钱「已转出未转入」；
2. **commit_ts 跨快照错乱**：同一事务两个 key 用了不同 commit_ts，
   审计快照 ts 恰好夹在中间 → 只看到一半改动；
3. **failover 后 timestamp 回退**：新 leader 发出旧 ts，新事务的提交
   被旧快照看见或旧提交被新快照漏掉；
4. **锁失效后双写**：TTL 误回滚了还活着的事务，但该事务后续 commit
   又成功 → 同一 key 两个事务的写都落库。

本测试对这四类 bug 都敏感：1/2 直接导致某次审计 sum ≠ S；3/4 在
kill leader 的窗口内高概率触发。

## 4. 残锁与「清锁遍历」的设计

混沌环境下残锁是**预期产物**而非 bug：

- 客户端 `Transaction::Commit` 在 Commit RPC 结果未知时（超时/连接断）
  **故意不回滚**——盲目回滚可能毁掉一个实际已提交的事务。残锁留给
  后续读者经 `CheckTxnStatus` 按 primary 的真实状态裁决；
- kill -9 leader 时，正在 Prewrite/Commit 中途的事务同样会留下锁。

因此压测结束后、离线检查之前，驱动程序做一次**清锁读遍历**：
逐账户开只读事务 Get（每账户最多 5 次，间隔 500ms）。`Transaction::Get`
内部撞锁即触发 resolve 链路：

```
撞锁 → CheckTxnStatus(primary, lock_ts)
  ├─ primary 已提交   → 用其 commit_ts 帮提交该 key（推进 secondary）
  ├─ 已回滚/TTL 过期  → TxnRollback 清残锁（写墓碑）
  └─ 还活着           → 指数退避后重试（锁 TTL 3s，重试预算足够等它过期）
```

这一步既是**功能验证**（resolve 链路在真实混沌残锁上跑通），也为
下一步「lock CF 应为空」的离线断言创造前提——没有这一步，lock CF
非空是正常现象，断言就没有意义。

## 5. 离线 CF 检查（mvcc_db_check）

集群 **停止后**，对每个节点的 RocksDB 用 `OpenForReadOnly` 直接检查
（绕过服务端，杜绝「服务端读路径掩盖存储层问题」的可能）：

### 检查 1：lock CF 为空
遍历 lock CF，任何残留条目都打印 key/primary/start_ts/ttl 并判 FAIL。
经过清锁遍历后仍残留 = resolve 链路有洞（某种锁没人能清掉）。

### 检查 2：write CF 无悬空 start_ts
write CF 的键是 `EncodeKey(user_key, commit_ts)`，值是
`WriteInfo{start_ts, kind}`。逐条校验：

| kind | 校验 |
|------|------|
| kPut | ① `start_ts < commit_ts`（时序）；② default CF 必须存在 `EncodeKey(user_key, start_ts)` 的数据版本（**悬空检查**） |
| kDelete | 无需数据版本（删除本来就不写 default CF） |
| kRollback | 跳过（墓碑，不指向数据） |

「悬空 start_ts」意味着提交记录指向一个不存在的数据版本——快照读
会 Seek 到这条 write 记录然后去 default CF 取数据取不到，等价于数据
丢失。可能根因：Prewrite 的数据写与 Commit 的 write 记录没有走同一个
原子 WriteBatch、或 apply 顺序错乱。

三个节点**逐一检查**：由于锁/提交/回滚全部经 Raft 日志复制，所有
副本 apply 后必须收敛到同一 CF 状态；任一 follower 上发现问题同样
是 bug（脚本在停集群前 sleep 3s 让 follower 追平日志）。

## 6. 判定标准汇总

| 项 | PASS 条件 |
|----|-----------|
| 在线审计 | audit_violation == 0（audit_retry 不限） |
| 最终审计 | 压测 + 清锁后，快照总额 == S |
| lock CF | 三节点全部为空 |
| write CF | 三节点全部：无悬空 kPut、无 start_ts >= commit_ts、键值可解码 |

脚本任一项 FAIL 即退出码 1；`bank_chaos_test` 自身违规也是退出码 1。

## 7. 与既有测试的关系（测试金字塔）

| 层级 | 测试 | 覆盖 |
|------|------|------|
| 单元 | `local_tso_test`（8 例） | TSO 单调/并发/预留上界纯逻辑 |
| 单元 | `mvcc_txn_test` / `mvcc_codec_test` | MVCC 存储层单机语义 |
| 集成 | `txn_integration_test` | 服务端事务 RPC（单节点，逐场景） |
| 集成 | `txn_client_test`（3 例） | 客户端 Transaction 库（冲突重试/TTL 回收/帮提交） |
| 集成 | `txn_failover_test.sh` | TSO failover 不重复不回退（3 节点定向 kill leader） |
| **混沌** | **`txn_bank_chaos_test.sh`** | **以上全部在随机故障下的组合正确性 + 存储层终态审计** |

前面各层验证「单条路径对不对」；混沌层验证「所有路径随机交错、
随机宕机时，全局不变量是否仍然成立」——这是分布式事务测试里
唯一能覆盖「未知未知」的一层（思路同 Jepsen 的 bank workload）。

## 8. 已知边界与可扩展方向

- **一次只杀一个节点**：majority 恒在。不覆盖「多数派同时丢失」
  （那属于可用性场景，已由 `tests/chaos/failover_test.sh` 测试 3 覆盖）；
- **审计是抽样而非全时序**：200ms 间隔的快照抽查，理论上存在
  「违规状态只在两次审计之间存在」的漏检窗口——但 MVCC 下已提交
  状态不会自愈，一旦不一致必然被后续审计与最终审计抓到；
- **只用 kill -9**：不注入网络分区/时钟跳变/磁盘错误。可扩展：
  `tc netem` 加延迟丢包、`clock_settime` 模拟时钟回拨（考验 TSO
  的 `RecoverTo` 路径）；
- **金额守恒但不验证单账户余额非负的强约束**：转账前有余额检查，
  但两个并发事务基于同一快照都判定「余额足够」时，乐观锁会让
  其中一个 Commit 失败（写写冲突），所以不会双花——这本身也是
  被测行为的一部分；
- 可加大压强：`--accounts` 调小（冲突率升高）、`--threads` 调大、
  混沌间隔调短（`KILL_INTERVAL_S`）。
