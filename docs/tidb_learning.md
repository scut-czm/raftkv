# TiDB / TiKV 中值得数据库内核研发学习的设计（TSO 之外）

> 括号内标注了与当前 raftkv 代码的对应关系，便于对照演进。

## 1. Percolator 事务 + Async Commit / 1PC
- 已实现：标准 2PC（prewrite / commit / rollback / resolve）、primary lock、lock/write/default 三列族思路。
- 值得学：**Async Commit / 1PC** 如何减少一次 TSO/网络 RTT——primary 提交即可返回，仍保证线性一致与可恢复性。
- 对应代码：`kv_service.cc` 的 `TxnPrewrite/TxnCommit/TxnRollback/CheckTxnStatus`，目前是标准 2PC。

## 2. MVCC + GC safepoint
- 值得学：GC safepoint 如何与"最老快照读"、TSO 联动；write CF 存 `commit_ts → start_ts` 指针的编码；resolve lock 与 GC 的配合。
- 对应代码：已有 `mvcc_txn`，但（从注释看）缺少 GC / safe-ts。

## 3. MultiRaft / Region 分片 + split/merge
- 从"单 Raft 组"走向"可水平扩展存储引擎"的关键：Region 自动分裂/合并、动态迁移。
- 对应代码：当前是单组，TSO 全局唯一性也只在单组内成立；这是最值得的下一步演进方向。

## 4. Raft 工程化细节
- ReadIndex / Lease Read（已用：`is_leader_lease_valid` + applied barrier）。
- Log 复制与 apply 解耦、批量/流水线 apply、async write（已做：`on_apply` 用 WriteBatch 合并落盘 + closure 移交 bthread）。
- Joint Consensus 成员变更。
- Snapshot（已用：RocksDB checkpoint 硬链接，与 TiKV 同思路）。
- **Follower Read / Stale Read + safe-ts**（代码注释明确写了"当前没有 follower safe-ts"，正是可补的坑）。

## 5. PD 调度器
- 基于 label 的副本放置、hot region 调度、store 负载均衡、region scatter、heartbeat 收集。
- TSO 只是 PD 的一小块，**调度才是 PD 的核心价值**。

## 6. Coprocessor 计算下推
- 把过滤/聚合下推到 TiKV，减少数据回传——存算协同的经典范式。

## 7. 存储引擎调优
- RocksDB 多 CF、compaction 调优。
- **Titan（KV 分离）**：解决大 value 场景的写放大。

## 8. 时钟与混合时间戳
- HLC / TSO 与墙钟的关系、时钟漂移容忍。
- 对应代码：`HybridNowTs()` 已是与 TSO 同构的布局（phys<<18 | logical）。

## 9. 分布式正确性验证
- failpoint 注入、混沌测试、Jepsen 式线性一致性校验。
- "证明它对"往往比"写出来"更难、更值得学。

## 10. 存算分离 / 云原生（TiDB Serverless）
- 共享存储 + 无状态计算层的演进方向。

---

## 贴合当前 raftkv 的优先级建议
1. 补 **follower safe-ts / stale read**（代码里已留坑）。
2. 加 **MVCC GC**（safepoint + resolve lock）。
3. 演进到 **MultiRaft / Region 分片**。
