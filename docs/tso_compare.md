# 你的 LocalTso vs TiDB PD TSO 对比

## 0. 一句话结论

你的实现是 **"把 TiDB PD 的 TSO 哲学（内存做吞吐、限额做安全、failover 从持久化上界续发）搬进单个 Raft 组的 Leader 里"**。
编码、单调性、预留窗口、failover 单调性这四点的**设计精髓一致**；
主要差异在于**部署拓扑**（内嵌 KV 组 vs 独立 PD 集群）和**预留窗口的度量方式**（按"号数/批"续约 vs 按"时间 3s"续约）。

---

## 1. 相同的设计精髓

| 精髓 | TiDB PD | 你的 LocalTso | 是否一致 |
|---|---|---|---|
| 位运算拆分 physical/logical | `physical<<18 \| logical` | `NowMs()<<18 \| logical`（`kLogicalBits=18`） | ✅ 完全一致（**注意：真实 TiDB 也是 18 位逻辑位，不是你正文里写的 16 位**） |
| 快路径 0 I/O | 只要没超过 `max_persisted_time`，内存分配 | 只要没超过 `tso_reserved_`，`NextBatchBounded` 纯 CAS | ✅ 一致 |
| 内存吞吐 + 限额安全 | 预分配窗口 | 预留上界 `tso_reserved_` | ✅ 一致 |
| failover 不回退 | 新 leader 读 etcd 的 `max_persisted_time`，从 +1 续发 | 新 leader `on_leader_start` 里 `RecoverTo(tso_reserved_)` | ✅ 一致 |
| 批量发号 | client pipelining，PD 一次切一段 | `NextBatch/NextBatchBounded` 一次 CAS 跳 count，返回 `[start,start+count)` | ✅ 服务端一致 |

你甚至比 TiDB 更"无锁"：分配走 `compare_exchange_weak` CAS 循环，而 PD 内部是 `sync.Mutex` + 原子指针换窗口。

---

## 2. 关键差异

### 2.1 部署拓扑：内嵌 vs 独立 PD  ← 最本质的区别
- **TiDB**：TSO 在 **PD 这个独立集群**里，用 **etcd 自己的 Raft** 持久化边界，为**整个集群全局**发号。
- **你的**：TSO 跑在 **KV 业务 Raft 组的 Leader** 上，`OP_TSO_RESERVE` 走的是**同一个 KV Raft 组**、落 RocksDB `meta` CF。
- **影响**：
  - 你的 TSO 续约延迟和**数据写入共用一条日志**——KV 组 commit 抖动/堆积时，TSO 续约也会被拖慢；PD 把 TSO 与数据写彻底隔离。
  - 你目前是**单组**，全局唯一性只在这个组内成立；多组/多 region 仍需要真正的 PD。
  - 代码注释已点明这是**过渡设计**（"M6 换成全局 PD TSO 时 ts 比较语义和存储格式完全不变"）——刻意让编码同构，这是很好的前瞻。

### 2.2 预留窗口的度量：号数 vs 时间  ← 第二本质区别
- **TiDB**：持久化的是**纯物理时间** `max_persisted_time = now + 3s`（时间维度）。有**后台定时器**随墙钟推进主动刷新，所以即使空闲一段时间，新 leader 也能立刻从 `persisted+1` 服务。
- **你的**：持久化的是**已移位的 ts 值**，`new_reserve = max(Current, HybridNowTs()) + count + kTsoReserveBatch`（号数维度），且**只在"这次发号会越过上界"时按需续约**，没有后台定时预推进。
- **影响 / 可改进点**：
  - 冷启动或空闲后第一笔越界发号，会**同步付一次 Raft round-trip**；TiDB 用 3s 定时器把这个成本摊平了。
  - 建议加一个**周期性预留刷新**（类似 PD 的 3s timer），把同步续约变成后台异步续约。
  - 窗口按"号数"度量时，`kTsoReserveBatch` 若相对负载偏小，会频繁写 Raft；TiDB 的时间窗口天然随时间伸缩、与吞吐解耦。

### 2.3 正确性不变式的实现
- 你的核心不变式是"**已发出的号 ≤ 已持久化上界**"，靠 `NextBatchBounded` 把 **越界检查和 CAS 放在同一个循环**里实现（`if (next > bound) return false;` 就在 CAS 之前），并发下不会漏发任何越界号——这点做得很干净、很关键。
- 双保险：`on_apply` 里 follower 对 `OP_TXN_COMMIT` 也做 `tso_.RecoverTo(req.commit_ts())`，让 follower 的水位跟着日志走，failover 后新 leader 起点不低于任何已提交的 commit_ts。TiDB 不需要这条（因为发号方就是 PD 自己）。

### 2.4 与事务（Percolator）的耦合
- 你在 `TxnCommit` 里：`commit_ts==0` 时用 `AllocateTso` 现分配，并且 `if (ts <= start_ts) 拒绝`，防止伪造/超前的 `start_ts`；注释明确说明**不能用 `start_ts+1` 兜底**（会突破预留上界、破坏 failover 不变式）——这个取舍是对的。
- TiDB 的 PD 是**通用发号器**，start_ts/commit_ts 的分配逻辑在 TiDB server 侧，PD 不关心事务语义。你把两者揉在一个服务里，简单但耦合更强。

---

## 3. 需要注意的风险 / 建议

1. **TSO 与数据写共用 Raft 组**：高写入负载会拖慢 TSO 续约。长期方案就是注释里说的独立 PD；短期可考虑给 `OP_TSO_RESERVE` 更大的批量、或后台预续约以减少同步写。
2. **无后台预推进**：加一个定时器在水位接近 `tso_reserved_` 时提前异步续约（TiDB 的做法），避免尖刺延迟。
3. **墙钟依赖**：`HybridNowTs()` 用 `gettimeofday_ms`，向后跳有 `max(phys, last+…)` 兜底不回退；但**向前大跳**会一次性吃掉大量未来物理时间（把 ts 冲到墙钟前面）。可加对物理时钟前跳幅度的告警/上限。
4. **`kMaxTsoBatch = 1<<18` 的意义**：限制单次 count，避免逻辑位借位冲进物理位——这点你已经处理了，值得保留注释说明。
5. **重试上限 4 次**：时钟快进 + 高并发预留竞争时可能返回 `EAGAIN`。确认调用方对 `EAGAIN` 有退避重试。
6. **正文口误**：你正文写"低 16 位逻辑计数器 / 2^16=65536"，但真实 TiDB 和你的代码都是 **18 位（2^18=262144）**。对外讲的时候统一成 18 位更准确。

---

## 4. 一张速查表

| 维度 | TiDB PD | 你的 LocalTso |
|---|---|---|
| 位布局 | phys<<18 \| logical(18) | phys<<18 \| logical(18) |
| 运行位置 | 独立 PD 集群 | KV Raft 组 Leader 内嵌 |
| 边界持久化介质 | etcd(独立 Raft) | 同一 KV Raft 组 + RocksDB meta CF |
| 边界度量 | 物理时间(默认 +3s) | ts 号数(count + kTsoReserveBatch) |
| 边界刷新时机 | 后台定时器主动 | 越界时按需(同步一次 Raft) |
| 分配同步原语 | Mutex + 原子换窗 | 无锁 CAS 循环 |
| 批量发号 | client pipelining + PD 切段 | count 参数 + NextBatch 切段 |
| failover 续发 | 读 etcd persisted，+1 起 | RecoverTo(tso_reserved_) |
| 全局唯一范围 | 整集群 | 当前单组 |
| 事务耦合 | 无(通用发号) | 有(TxnCommit 内分配 + 防伪造校验) |
