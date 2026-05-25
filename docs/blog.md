# 从零实现分布式 KV 存储：RaftKV 技术解析

> 基于 **braft + RocksDB**，8 天从零实现，达成 **TPS = 19,488（含故障注入）**、**读 P99 = 1.9ms**、**故障切换 < 3s** 的生产级指标。
>
> 项目地址：`https://github.com/yourname/raftkv`

---

## 1. 为什么需要分布式 KV？

### 1.1 单机存储的两座大山

无论是 Redis 单节点，还是 RocksDB / LevelDB 这样的本地嵌入式存储，单机方案都绕不开两个根本问题：

- **单点故障（SPOF）**：进程挂了、机器宕机，服务立刻中断
- **数据丢失**：磁盘损坏、数据中心断电，数据可能永久不可恢复

对一般业务这或许可以接受，但在**金融信创**场景下这是不可接受的——
监管要求**至少 3 副本**、**RPO=0（零数据丢失）**、**RTO < 30s**。

### 1.2 分布式 KV 的解法

通过**多副本 + 共识协议**同时解决两个问题：

| 问题       | 解法                                                       |
| ---------- | ---------------------------------------------------------- |
| 单点故障   | 数据复制到 N 个节点，任一节点宕机不影响服务                |
| 数据一致性 | Raft / Paxos 共识协议，写入需多数派确认                    |
| 自动恢复   | Leader 故障 → Follower 选举新 Leader → 客户端自动重定向    |

**RaftKV** 是这条路线的完整实践：3 副本部署，最多容忍 `(N-1)/2` 个节点同时故障。

---

## 2. 整体架构设计

### 2.1 四层架构

```
┌──────────────────────────────────────────────────┐
│             Client SDK (KvClient)                 │
│      自动 redirect + 重试 + 线性一致读             │
└────────────────────┬─────────────────────────────┘
                     │ brpc RPC (Protobuf)
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
┌─────────┐   ┌─────────┐   ┌─────────┐
│ Node 0  │   │ Node 1  │   │ Node 2  │
│ :8200   │◄──│ :8201   │◄──│ :8202   │
│ (Leader)│   │(Follower│   │(Follower│
├─────────┤
│KVService│ ← RPC 入口（Leader 检查 + redirect）
├─────────┤
│  braft  │ ← Raft 共识（日志复制 + 选举 + 快照）
├─────────┤
│KVState  │ ← 状态机（应用日志到存储）
│Machine  │
├─────────┤
│ RocksDB │ ← 存储引擎（data CF + meta CF）
└─────────┘
```

### 2.2 各层职责

| 层              | 技术选型             | 核心职责                              |
| --------------- | -------------------- | ------------------------------------- |
| **Client SDK**  | brpc Channel         | 自动 redirect、超时与重试             |
| **RPC 层**      | brpc + Protobuf      | 接收请求、Leader 检查、区分读写路径   |
| **共识层**      | braft（百度 Raft）   | 日志复制、选举、快照管理              |
| **状态机层**    | KVStateMachine       | `on_apply` / `on_snapshot_save/load`  |
| **存储层**      | RocksDB              | 持久化 KV 数据，多 Column Family      |

### 2.3 关键设计决策

1. **职责分离**：Raft log 由 braft 管理，RocksDB 只存应用层数据，避免双重持久化
2. **快照优化**：RocksDB Checkpoint（硬链接），O(1) 完成，不阻塞写入
3. **读路径分层**：弱一致读（直读状态机）+ 线性一致读（Leader Lease Read）
4. **写路径异步**：通过 `Closure` 回调，避免线程阻塞

---

## 3. 关键实现：状态机 + RocksDB

### 3.1 状态机改造：从 std::map 到 RocksDB

braft 官方示例用 `std::map` 仅用于演示，重启即丢失。

**改造前**：

```cpp
// 内存 map，重启丢失
std::lock_guard<std::mutex> lock(mutex_);
data_[key] = value;
```

**改造后**：

```cpp
// RocksDB 持久化，内部已有锁，无需外层 mutex
storage_->Put(key, value);
```

### 3.2 快照优化：从 O(n) 序列化 到 O(1) 硬链接

这是 RaftKV **最有价值的优化**。

#### 旧方案的问题

```cpp
// 旧版 on_snapshot_save：遍历 map 写文本
for (auto& [k, v] : map_) {
  file << k << "\t" << v << "\n";  // O(n) 序列化
}
// 10GB 数据 = 写 10GB 文本 ≈ 数十秒，期间阻塞写入
```

#### 新方案：RocksDB Checkpoint

```cpp
void KVStateMachine::on_snapshot_save(SnapshotWriter* writer, Closure* done) {
  std::string path = writer->get_path() + "/rocksdb_checkpoint";
  storage_->CreateCheckpoint(path);          // 毫秒级
  writer->add_file("rocksdb_checkpoint/");
  done->Run();
}
```

**为什么这么快？**

RocksDB SST 文件是**不可变的**。Checkpoint 不复制任何数据，只为每个 SST 文件创建**硬链接**——两个目录项指向同一个 inode，磁盘上数据只有一份。

```
原数据目录：              Checkpoint 目录：
  000010.sst ─┐            000010.sst ─┐
  000012.sst ─┤(硬链接)     000012.sst ─┤(硬链接)
              ▼                          ▼
        同一个 inode（零拷贝）
```

| 方案                | 时间复杂度 | 写入阻塞 | 10GB 实际耗时 |
| ------------------- | ---------- | -------- | ------------- |
| 遍历 map 写文本     | O(n)       | 是       | 数十秒        |
| RocksDB Checkpoint  | O(文件数)  | **否**   | **< 100ms**   |

### 3.3 线性一致读：从 ReadIndex 翻车 到 Leader Lease Read

最初按 ReadIndex 思路实现，但性能测试时发现：

```
读 avg 延迟 ≈ 写 avg 延迟（~4ms）
```

**根因**：误把 `OP_GET` 写进了 Raft 日志，读操作被当成写操作走了一遍共识！

修复后改用 **Leader Lease Read**：

```cpp
void KVServiceImpl::Get(...) {
  if (request->linearizable()) {
    if (node_->is_leader_lease_valid()) {
      // Lease 有效：当前 Leader 一定还是 Leader → 直接读
      fsm_->Get(request->key(), &value);
      return;
    }
    response->set_redirect(GetLeaderAddr());
    return;
  }
  fsm_->Get(request->key(), &value);  // 弱一致：直读
}
```

**效果**：读 P99 从 **28ms → 1.9ms**（降幅 93%）。

---

## 4. 性能优化实践

完整调优路径，TPS 从 4K 提升到 16K：

| 阶段 | 关键改动                                              | 纯写 TPS   | 读 P99      |
| ---- | ----------------------------------------------------- | ---------- | ----------- |
| ①    | Debug 编译                                            | 4,044      | 28ms        |
| ②    | `RelWithDebInfo` + 关闭 RocksDB WAL + 批量写          | 4,759      | 28ms        |
| ③    | `--raft_enable_leader_lease=true`                     | 4,759      | **1.9ms** ✅ |
| ④    | `--raft_sync=false` + 4MB append buffer               | **11,067** | 1.9ms       |
| ⑤    | 客户端 threads 16 → 32                                | **15,973** | 1.9ms       |
| ⑥    | `level0_slowdown=40` + `level0_stop=80`               | **16,357** | 1.9ms       |

### 4.1 关闭 fsync：最大单点提升

`raft_sync=false` 让 Raft log 走 page cache，由 OS 异步刷盘——**TPS 直接翻倍（+132%）**。

**为什么安全？** 3 副本下，只要不同时掉电，数据就不丢。牺牲极小概率耐久性换吞吐。

### 4.2 RocksDB 调优要点

```cpp
options.write_buffer_size = 64 << 20;          // 64MB MemTable
options.max_write_buffer_number = 4;
options.max_background_jobs = 8;
options.level0_slowdown_writes_trigger = 40;   // 默认 20
options.level0_stop_writes_trigger = 80;       // 默认 36
table_opts.block_cache = NewLRUCache(256 << 20);
table_opts.filter_policy.reset(NewBloomFilterPolicy(10, false));
write_opts_.disableWAL = true;                 // braft 已保证持久性
```

调优 ⑥ 把 P999 从 **63ms 降到 16ms**——毛刺直接影响业务体感。

### 4.3 Pipeline 复制（braft 原生）

多个 LogEntry **并发飞行**，不等上一条 ack 就发下一条；配合 4MB 批次进一步降低系统调用次数。

### 4.4 最终基准结果

| 场景                  | 线程数 | TPS                  | P99 延迟              | 错误率 |
| --------------------- | ------ | -------------------- | --------------------- | ------ |
| 纯写（1KB value）     | 32     | **16,357**           | ~5ms                  | 0%     |
| 7:3 读写混合          | 16     | 写 7,522 / 读 3,227  | 写 5.42ms / 读 9.04ms | 0%     |
| Scan（range=100）     | 4      | 3,927                | 1.81ms                | 0%     |

---

## 5. 混沌测试：验证容错能力

### 5.1 测试场景

| 场景                               | 预期             | 实际       |
| ---------------------------------- | ---------------- | ---------- |
| Kill Leader                        | < 3s 切换        | **< 3s** ✅ |
| Kill 1 Follower                    | 写入不受影响     | PASS       |
| Kill 2 Follower（majority 丢失）   | 写入阻塞         | PASS       |
| 恢复 Follower                      | 写入恢复         | PASS       |
| 连续多次 Kill Leader               | 每次 < 3s        | PASS       |

### 5.2 高强度压测 + 故障注入

**测试条件**：
- 32 客户端线程持续写入，1KB value
- 压测期间随机 Kill Leader **2 次**，5s 后重启
- 持续 60 秒

**结果**：

| 指标                | 数值                          |
| ------------------- | ----------------------------- |
| TPS（含 2 次 Kill） | **19,488**                    |
| 失败请求数          | **0**                         |
| Leader 切换恢复     | < 3s                          |
| 磁盘                | 稳定（snapshot 120s 自动截断）|

> 客户端 SDK 的**自动 redirect + 重试**机制是 fails=0 的关键——
> 节点切换期间 RPC 失败会被自动捕获并切到新 Leader，对业务完全透明。

### 5.3 一个真实踩坑

测试初期遇到一个**死锁式故障**：压测 18 秒后所有节点 step_down，集群完全卡死。

**排查链路**：

```
snapshot_interval_s=3600（1 小时一次）
  ↓ Raft log 无法截断，疯狂膨胀
磁盘写满 → ENOSPC
  ↓ braft 触发 on_error
leader step_down → 但 step_down 又需要写 log → 死锁
```

**修复**：`snapshot_interval_s` 改为 **120s**，让 Raft log 能定期被快照截断。

这个 Bug 让我深刻理解了——**生产系统的稳定性，往往不是被功能 Bug 干掉的，而是被资源耗尽 + 错误处理路径的死锁干掉的。**

---

## 6. 总结与展望

### 8 天交付清单

- ✅ 完整 CRUD API（Put / Get / Delete / Scan）
- ✅ Raft 共识 + 3 副本，自动故障切换
- ✅ RocksDB 持久化 + 毫秒级快照（Checkpoint 硬链接）
- ✅ Leader Lease Read 线性一致读（P99 = 1.9ms）
- ✅ 客户端 SDK 自动 redirect + 重试
- ✅ TPS = 19,488（含故障注入），故障切换 < 3s
- ✅ 完整文档：README + ARCHITECTURE + CONTRIBUTING + 性能报告

### 核心收获

1. **正确性 > 性能**：Lease Read 翻车之前，所有"性能数据"都是假的
2. **快照机制是分布式存储的命门**：选错快照实现，系统永远跑不快
3. **混沌测试不是可选项**：Bug 不在代码里，在资源边界和错误处理里

### 下一步方向

- **SQL 层**：在 RaftKV 之上构建 SQL 引擎（Parser → Optimizer → Executor）
- **分布式事务**：Percolator 模型，支持跨 Key 事务
- **Multi-Raft**：数据分片，支持水平扩展
- **Raft Learner**：只读副本，分担读压力

---

**项目地址**：`https://github.com/yourname/raftkv`

**技术栈**：C++17 · braft · brpc · RocksDB · Protobuf · CMake

如果觉得有帮助，请 Star 支持！欢迎 Issue / PR 交流。

---

> **标签**：`分布式系统` `Raft` `RocksDB` `C++` `KV存储` `共识协议`
