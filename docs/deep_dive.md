# RaftKV 主攻深挖点（社招级追问准备）

> 完整自研库的短板是"每块都浅"。策略：**选 3 个点砸到底层**，其余一句带过。
> 下面 3 个点都是你代码里真实存在、且能层层深挖到"面试官问不动"的：
> 1. 单调 TSO + failover 不回退
> 2. Percolator 事务的确定性 apply + 残锁自愈
> 3. 线性一致读三档 + applied barrier
> 每个点给：**电梯稿 → 逐层追问链（能答到第几层决定你的段位）→ 白板要点 → 对标大厂**。

---

## 主攻点 1：单调 TSO + failover 不回退 ⭐（最推荐做"最深的那一个"）

**为什么选它**：最能体现分布式底层功底，代码只有 ~80 行但能问出 5 层深度，且和你金融背景（对账/审计要求时间戳严格单调）强相关。

**电梯稿**
> 我实现了一个单调时间戳发号器，布局同构 TiDB（物理毫秒<<18 | 逻辑位）。核心不变式是"**已发出的号 ≤ 已持久化的预留上界**"。发号在内存里用 CAS 完成、零 I/O；只有当发号要越过预留上界时，才走一条 Raft 日志把更大的上界持久化，之后再发。Leader 切换时新 leader 从持久化上界续发，绝不回退。

**逐层追问链**
- **L1 布局**：为什么 phys<<18？→ 高位物理时间保证跨毫秒天然递增，低 18 位逻辑计数支持同毫秒内 2^18 个号；ts 直接按整数比大小就是时间序。
- **L2 单调**：并发下怎么保证严格递增？→ `Next()` 用 `next=max(phys,last+1)` + `compare_exchange_weak` 循环；CAS 失败重算，保证每个号被唯一分配且严格增。
- **L3 为什么需要预留上界**：不落盘不行吗？→ 若每个号都落盘，性能废掉；若完全不落盘，宕机后新 leader 不知道旧 leader 发到哪 → 可能重发。所以**低频持久化"上界"，高频在界内内存发号**（内存做吞吐、限额做安全）。
- **L4 failover 不回退（关键）**：新 leader 为什么不能从 now() 起？→ 新 leader 墙钟可能比旧 leader 慢，从 now() 起会发出比旧 leader 已发过的更小的号 → 时间倒退。所以 `on_leader_start` 里 `RecoverTo(已持久化上界)`，从上界续。
- **L5 并发正确性（最深）**：怎么保证任何时刻都不发出超过已持久化上界的号？→ `NextBatchBounded` 把"越界判断 `if(next>bound) return false`"和"CAS"放在**同一个循环体**内——任何 CAS 成功的号一定 ≤ bound。上界不足时提交 `OP_TSO_RESERVE` 日志、apply 后重试，最多重试 4 次防活锁。预留批大小 `kTsoReserveBatch = 2^16`，即"每发 6.5 万个号才写一次 Raft 日志"，把持久化频率压到极低。
- **L6 加分（主动说局限）**：我这是内嵌在 KV Raft 组、按"号数"续约、越界时同步续；TiDB PD 是独立集群、按"物理时间 3s"续、后台定时预推进。我的短板是 TSO 与数据写共用日志、只单组全局唯一——但我刻意让编码同构 TiDB，方便以后无缝换全局 PD。

**白板要点**：画一条数轴，标 `last`(当前水位)、`tso_reserved_`(已落盘上界)；演示 leader 挂掉 → 新 leader RecoverTo(上界) → 新号从上界之后开始，可视化"不回退"。

**对标**：TiDB PD TSO、Google TrueTime（用时间不确定区间 vs 你用持久化上界，殊途同归解决"分布式全局时间"）。

---

## 主攻点 2：Percolator 事务 —— 确定性 apply + 残锁自愈 ⭐

**为什么选它**：金融转账原子性的直接体现；能问到"副本一致性""崩溃恢复""并发冲突"三个硬核方向。

**电梯稿**
> 我按 Percolator 实现分布式事务：三列族 default(数据) / lock(活跃锁) / write(提交记录)。2PC：Prewrite 写 default+lock 并检测写冲突，Commit 写 write CF 删 lock。所有写**必须在 Raft on_apply 里执行**以保证各副本确定性一致。primary 挂了留下的残锁由后续事务通过 CheckTxnStatus+ResolveLock 自愈。

**逐层追问链**
- **L1 三 CF 编码**：读怎么找到正确版本？→ write CF 的 key 是 `Encode(user_key, ~commit_ts)`（对 commit_ts 取反编码），所以 `Seek(key, snapshot_ts)` 的落点就是"commit_ts ≤ snapshot_ts 的最新版本"，一次 seek 搞定快照读；再拿它的 start_ts 回 default CF 取数据。
- **L2 快照隔离**：读时怎么处理锁？→ 若存在 `start_ts ≤ snapshot_ts` 的锁，说明该版本命运未定，返回 `kLocked` 让客户端 backoff/resolve；**更晚的锁不影响本快照**（它即便提交 commit_ts 也 > snapshot_ts）。这个"锁的时间戳比较"是 SI 正确性的关键。
- **L3 写冲突**：Prewrite 怎么防丢更新？→ 若发现 write CF 有 commit_ts ≥ 自己 start_ts 的记录 → `kWriteConflict`，事务重试；有别人的锁 → `kLocked`。
- **L4 为什么写必须在 on_apply 里（副本一致性核心）**：→ 三副本对**同一条 Raft 日志**必须产生**完全相同**的状态变更，否则副本分叉。所以像 TTL 判定用的 `now_ts`、commit_ts 都由 **leader 定好、随日志复制**，而不是各副本各自取本地时间——否则同一条日志在不同副本上判 TTL 结果不同就分叉了。这是我认为整个事务实现里最容易被忽略、也最能体现功底的一点。
- **L5 残锁自愈**：primary 宕机锁清不掉怎么办？→ 别的事务撞锁 → CheckTxnStatus 判 primary 命运：已提交→帮 secondary 提交；已回滚/TTL 过期→回滚（写 rollback 墓碑）；TTL 未过→返回剩余 TTL 让调用方等。**迟到的 Prewrite 撞上 rollback 墓碑**要能识别并拒绝（幂等），我用 GetTxnRecord 按 start_ts 精确匹配处理。
- **L6 崩溃恢复**：apply 到一半宕机？→ on_apply 里我把一批操作累积进单个 WriteBatch **原子落盘**，落盘成功才推进 applied_index、才应答客户端；失败则整批不推进，靠 Raft 重放保证 exactly-once 语义。

**白板要点**：画一次转账 A→B 的时序：prewrite(primary=A) → prewrite(B) → commit(A) → commit(B)；再画"commit A 后 crash"，展示 B 的锁如何被后续事务 resolve。

**对标**：Google Percolator 论文、TiKV 的 txn 模块、unistore mvcc.go（你注释里就对标了它的四分支）。

---

## 主攻点 3：线性一致读三档 + applied barrier ⭐

**为什么选它**：读一致性是面试高频，且能体现"性能与正确性分层兜底"的工程判断力。

**电梯稿**
> 读我做了三档：弱读直读状态机（最快，可能读到旧值）；Lease Read（租约有效期内直读，省一轮 Raft）；租约失效或 applied 落后时降级为 Log Read（提交一条只建顺序点的 OP_GET 日志，apply 后读）。三档之上都加了 applied barrier。

**逐层追问链**
- **L1 为什么写要共识、读可以不用**：→ 读不改变状态，只要保证读到的是"读发起时刻已 committed 的最新值"就线性一致；关键是确认 leadership + 状态机追上 commit 点。
- **L2 Lease Read 为什么安全**：→ 租约期内 leader 保证自己是唯一 leader（假设时钟不回拨、租约 < 选举超时），期内 committed 数据一定可见。省掉了 ReadIndex 的一轮心跳 RTT。
- **L3 applied barrier 干嘛的（关键）**：→ leader 确认了 committed_index，但状态机 apply 是异步的，可能还没 apply 到那条。所以我 `WaitApplied(committed_index, 50ms)` 等状态机追上再读，否则会读到旧值——**committed ≠ applied 是很多人漏掉的点**。
- **L4 barrier 超时怎么办**：→ **绝不能超时后"凑合直读"**（破坏线性一致），我降级到 Log Read：走一条 Raft 日志，用它的 apply 顺序点保证读到不早于该点的状态。
- **L5 加分（主动说缺口）**：我目前 follower 不提供强读（没有 follower safe-ts），所有强读集中在 leader。要扩展读吞吐就得引入 safe-ts 做 follower read / stale read——这是我规划的下一步。

**白板要点**：画时间线：Client 发读 → leader 记 committed_index=C → 状态机 applied 从 A 追到 C → 读；对比"不等 barrier 直接读"会读到 <C 的旧值。

**对标**：etcd/TiKV 的 ReadIndex、TiKV 的 Lease Read 与 follower read safe-ts。

---

## 怎么串成你的"转行叙事"

你从服务器/后端转内核，独特优势是**系统/并发/网络的底层功底可迁移**，把它显式接到内核点上：
- "我原来做高并发服务，习惯用无锁/原子做热点路径" → 正好对应 **TSO 的 CAS 无锁发号**、on_apply 里用 atomic + memory_order 控制可见性。
- "我熟悉 RPC/线程模型" → 对应 **brpc/bthread**、closure 移交 bthread 避免阻塞 FSM 线程。
- "我关注延迟/吞吐权衡" → 对应 **Lease Read 省 RTT、批量 apply、TSO 内存发号、fsync 安全档 vs 高吞吐档的取舍**。

**面试主线建议**：主攻 **TSO（挖到 L6）+ 事务确定性 apply（挖到 L4/L5）**，读一致性作为第二梯队（挖到 L4），SQL 引擎只作广度证明。这样你既有"完整栈的架构观"，又有两个"社招级深度"的锚点，完整自研库的"浅"就被补上了。
