# RaftKV / RaftSQL 面试准备 —— 实现讲解 · 取舍对比 · 验证 · 简历修改建议

> 结合你项目当前真实实现（Raft+braft、MVCC Percolator、LocalTso 预留发号、SQL 火山模型引擎）与金融背景整理。
> 原则：**只讲你代码里真有的东西，把取舍和验证讲透，比堆量化指标更能打动数据库内核面试官。**

---

## 一、简历诊断（先改这里，否则面试会翻车）

当前简历 bullet 有几个**会被面试官当场击穿的诚实性风险**，务必先改：

### 🔴 必须改（自相矛盾/夸大，风险最高）
1. **"某银行核心资金清算系统依赖 Oracle…迁移至自研数据库"**
   - 问题：把个人学习项目包装成"银行生产系统迁移"，面试官一句"这是你在哪家银行做的、上线了吗、谁用了"就崩。
   - 改法：诚实定位为**"面向金融信创场景、自研的分布式事务 KV/SQL 数据库（个人项目）"**，动机可以写"受金融核心系统国产化替代趋势启发"，但不要谎称真实上线。

2. **"TPS 从 3K 提升至 15K（5 倍）、支撑日均千万级交易、全年可用性 99.99%"**
   - 问题：3K→15K 是拿你的 KV 和 Oracle 做**苹果比橘子**；"日均千万级""全年 99.99%"是生产口径，个人项目给不出。
   - 改法：只写**你实测的、可复现的**数字，并**带测试环境**：如"单机 3 节点、32 线程纯写实测 TPS ≈ 16k，P99 ≈ 5ms（AMD 6800HS / 60s 压测）"。

3. **🔴🔴 最致命的矛盾：`--raft_sync=false` + RocksDB `disableWAL=true` 却声称"金融资金清算"**
   - 问题：你 README 的调优里关了 Raft log fsync 和 RocksDB WAL 来冲 TPS。**这意味着宕机会丢已提交数据**——这跟"银行资金清算/强一致"是直接冲突的，面试官必问"你崩溃了钱账会不会丢"。
   - 改法（二选一，都要能讲）：
     - 诚实说明：**"高吞吐档"关 fsync 是为跑基准，"安全档"开 `raft_sync=true` + WAL 才是金融场景的正确配置**，并给出两档的 TPS 对比——这反而是极好的"性能/安全取舍"素材。
     - 或直接把简历数字改成**开 fsync 的安全档**数字。

### 🟡 建议改
4. **"目标 500+ stars"**：aspirational 数字别写进简历，删掉。
5. **"设计 Pipeline 日志复制…延迟降低 40%"**：braft 本身就做 pipeline/batch，"我设计的"要谨慎；改成"基于 braft 的 pipeline + 批量 apply，并做了 X 调优，实测写 P99 从 A→B"。
6. **"简化版 Percolator（Prewrite/Commit）"**：你其实还实现了 **Rollback / CheckTxnStatus / ResolveLock / TTL 残锁清理 / TSO 发 commit_ts**，比"简化版"强很多，要写全，这是硬亮点。

### ✅ 可以放心保留/强化的真实亮点
- Raft 线性一致读（Leader Lease Read + applied barrier + 降级 Log Read）
- Percolator MVCC（default/lock/write 三 CF、快照隔离、残锁 resolve）
- **单调 TSO + 预留发号 + failover 不回退**（这是最能体现分布式功底的点）
- SQL 全链路：Lexer→Parser→AST→逻辑优化（谓词下推/列裁剪）→物理计划→火山模型执行（含并行聚合）
- RocksDB Checkpoint 硬链接快照、混沌 failover 测试

---

## 二、各模块面试问答（怎么讲 + 追问 + 取舍 + 验证）

### 模块 1：Raft 与线性一致读

**主动讲（30 秒电梯稿）**
> 写走 braft 多数派共识；读我实现了三档一致性：弱读直读状态机、Lease Read（租约有效期内直读，避免走一轮 Raft）、租约失效或 applied 落后时降级为 Log Read（提交一条只建顺序点的 OP_GET 日志，在 apply 后读）。

**常见追问 & 答法**
- Q：Lease Read 为什么安全？→ 租约期内 leader 保证自己仍是唯一 leader（时钟不回拨假设），期内已 committed 的数据一定可见；我还加了 **applied barrier**（`WaitApplied(committed_index)`）保证读到的状态机确实追上了 commit 点。
- Q：barrier 超时怎么办？→ **不能超时后继续直读**（会破坏线性一致），我的代码是降级到 Log Read，而不是"凑合读"。
- Q：ReadIndex 和 Lease Read 区别？→ ReadIndex 要一轮心跳确认 leadership，更安全但多一个 RTT；Lease 用时间租约省掉这轮，代价是依赖时钟不漂移。

**取舍对比**：弱读（最快、可能读到旧值）↔ Lease Read（快、依赖时钟假设）↔ Log Read（最慢、最稳）。我默认用 Lease，失效再降级——**性能与正确性的分层兜底**。

**如何验证**：混沌测试随机 Kill leader + 32 线程压测 fails=0；可补一个"读到的值单调不回退"的断言测试。

---

### 模块 2：MVCC / Percolator 事务

**主动讲**
> 三列族：default 存 `(key,start_ts)→value`，lock 存活跃锁，write 存 `(key,commit_ts)→{kind,start_ts}`。2PC：Prewrite 写 default+lock 并做写冲突检测，Commit 写 write CF 删 lock。快照读按 snapshot_ts 在 write CF seek 最新 commit_ts≤ts 的记录再回 default 取值。残锁用 CheckTxnStatus（判 primary 命运 + TTL）+ ResolveLock 清理。

**常见追问 & 答法**
- Q：隔离级别？→ **快照隔离（SI）**。读用 start_ts 作快照，看不到未提交/更晚的提交。
- Q：写冲突怎么检测？→ Prewrite 时若发现 write CF 有 commit_ts ≥ 自己 start_ts 的记录 → `kWriteConflict`，事务重试；若有别人的 lock → `kLocked`，backoff 或 resolve。
- Q：primary 挂了锁怎么办？→ 别的事务撞到锁会 CheckTxnStatus：primary 已提交则帮 secondary 提交、已回滚则回滚、TTL 未过则等——这就是 Percolator 的**自愈**。
- Q：为什么所有写要在 on_apply 里做？→ 保证各副本对同一条日志产生**完全相同**的状态变更（确定性），否则副本会分叉。now_ts/commit_ts 都由 leader 定好随日志复制。

**取舍对比**：Percolator（去中心化、无独立事务管理器、锁下沉到 KV）↔ 传统集中式 2PC（有 TM 单点）。代价是**读要处理残锁**、依赖全局 TSO。

**如何验证**：单测覆盖 write-conflict / locked / rollback 幂等 / 迟到 prewrite 撞 rollback 墓碑；集成测试跑并发事务查最终一致。

---

### 模块 3：TSO（最能体现功底，重点准备）

**主动讲**
> 单调时间戳，布局与 TiDB 同构 `phys_ms<<18 | logical`。核心不变式：**已发出的号 ≤ 已持久化的预留上界**。发号在内存 CAS 完成（0 I/O）；只有当要越过预留上界时，才提交一条 OP_TSO_RESERVE 日志把更大的上界持久化（RocksDB meta CF）后重试。failover 时新 leader `RecoverTo(已持久化上界)`，绝不从 now() 重启，保证不回退。

**常见追问 & 答法**
- Q：failover 为什么不能从 now() 重启？→ 新 leader 墙钟可能比旧 leader 慢，从 now() 起可能发出比旧 leader 已发过的更小的号 → 回退。所以必须从持久化上界续。
- Q：并发下怎么保证不发越界号？→ `NextBatchBounded` 把"越界检查 `next>bound` 和 CAS"放在**同一个循环**里，任何 CAS 成功的号一定 ≤ bound。
- Q：和 TiDB PD 差别？→ 我是内嵌在 KV Raft 组、预留上界按"号数"续、按需同步续约；PD 是独立集群、按"物理时间（3s）"续、后台定时预推进。**见 docs/tso_compare.md**。

**取舍对比**：内嵌单组（简单、但 TSO 与数据写共用日志、只单组全局唯一）↔ 独立 PD（隔离、全局、复杂）。我刻意让编码同构，**便于以后无缝换成全局 PD**。

**如何验证**：可写一个"重启/切主后新号严格大于切主前最大号"的测试；并发多线程取号断言全局严格递增无重复。

---

### 模块 4：SQL 引擎（RaftSQL）

**主动讲**
> 全链路：Lexer→Parser 出 AST→逻辑计划→逻辑优化（**谓词下推、列裁剪**）→物理计划→**火山模型**执行（Open/Next/Close）。算子有 TableScan/Filter/Project/Aggregate/**并行聚合**/Limit。行编码 row_codec，表元数据 schema_manager，主键用 row_id_allocator。底层通过 adapter 走 RaftKV 事务接口。

**常见追问 & 答法**
- Q：火山模型缺点？→ 一次一行、虚函数调用开销大；改进方向是**向量化/批处理执行**（一次一批），可作为你"下一步"来讲。
- Q：谓词下推下推到哪？→ 目前下推到 TableScan 层过滤；真正的分布式数据库会**下推到存储节点（Coprocessor）**减少数据传输，这是我可演进的点。
- Q：事务怎么和 SQL 结合？→ DML 通过 adapter 映射到 KV 的 prewrite/commit，start_ts/commit_ts 来自 TSO。

**取舍对比**：火山模型（实现简单、易理解）↔ 向量化（吞吐高、实现复杂）。学习项目选火山模型合理，但要能说清升级路径。

---

## 三、结合金融背景怎么讲故事（面试叙事）

金融背景 + 分布式数据库是很好的组合，讲法：
1. **动机真实**：金融核心系统对**强一致、不丢数据、故障快速切换**要求极高，且有信创国产化替代诉求 → 我想亲手搞懂"一个满足金融要求的分布式事务数据库内核到底难在哪"，于是从零实现了 RaftKV/RaftSQL。
2. **把金融需求映射到技术点**：
   - 资金不能错/不能丢 → **强一致（Raft 多数派）+ 持久化（fsync/WAL 安全档）+ 快照隔离**。
   - 转账原子性 → **Percolator 2PC + primary lock 自愈**。
   - 全局一致的时间序 → **单调 TSO、failover 不回退**（对账/审计要求时间戳严格单调）。
   - 高可用 → **Leader 秒级切换、混沌测试验证**。
3. **主动亮取舍意识**（金融最看重）：我实测了"关 fsync 的高吞吐档 vs 开 fsync 的安全档"，**金融场景必须选安全档**——用这个体现你懂"性能不能以牺牲资金安全为代价"。

---

## 四、建议的简历改写（诚实高信号版）

> **RaftSQL —— 面向金融信创的分布式事务数据库（个人项目，C++17）**
>
> - **存储/事务**：基于 RocksDB(LSM) + MVCC 实现快照隔离；完整实现 Percolator 两阶段提交（Prewrite/Commit/Rollback + CheckTxnStatus/ResolveLock 残锁自愈 + 锁 TTL），事务时间戳由自研单调 TSO 分配。
> - **一致性**：基于 braft 的多副本共识；实现三档读一致性（弱读 / Leader Lease Read / 降级 Log Read）+ applied barrier，保证线性一致。
> - **单调 TSO**：物理+逻辑位同构 TiDB 布局，内存 CAS 发号 0 I/O、预留上界持久化、Leader 切换从持久化上界续发保证不回退。
> - **SQL 引擎**：Lexer→Parser→逻辑优化(谓词下推/列裁剪)→物理计划→火山模型执行（含并行聚合），支持 DDL/DML/聚合。
> - **性能与可靠性（单机 3 节点实测）**：安全档(开 fsync/WAL) 写 TPS ≈ X、P99 ≈ Y；高吞吐档 ≈ 16k TPS / P99 ≈ 5ms；混沌测试随机 Kill Leader、秒级切换、压测 0 失败。
> - **技术栈**：C++17 · RocksDB · braft · brpc · Protobuf。开源：github.com/scut-czm/raftkv
>
> （把 X/Y 补成你开 fsync 后实测的数字；如果暂时没测，先测一组再填。）

---

## 五、面试前 To-Do 清单
- [ ] 补一组**开 fsync/WAL 安全档**的性能数字（金融叙事必需）。
- [ ] 准备 TSO failover 不回退的白板图 + 不变式讲解。
- [ ] 能手画 Percolator 三 CF 的一次转账时序（prewrite→commit→残锁 resolve）。
- [ ] 想清楚火山模型→向量化、谓词下推→Coprocessor 两条"下一步"演进故事。
- [ ] README/简历口径统一（TPS、节点数、配置档）。
