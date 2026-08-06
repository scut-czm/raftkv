# 简历项目改写稿（RaftSQL）

> 目的：把"包装成银行生产系统 + 经不起追问的量化"改成"面向金融场景的自研项目 + 真实可复现数据 + 讲得出取舍"。
> 只需你补 1 处：**安全档(开 fsync) 的实测 TPS/P99**，跑法见文末。

---

## ✅ 可直接粘贴的简历版本

**RaftSQL —— 面向金融信创的分布式事务数据库（个人项目，C++17）**
`github.com/scut-czm/raftkv`

- **事务/存储**：基于 RocksDB(LSM)+MVCC 实现快照隔离；完整实现 Percolator 两阶段提交（Prewrite / Commit / Rollback + CheckTxnStatus / ResolveLock 残锁自愈 + 锁 TTL），事务时间戳由自研单调 TSO 分配。
- **一致性**：基于 braft 多副本共识；实现三档读一致性（弱读 / Leader Lease Read / 降级 Log Read）+ applied barrier 保证线性一致。
- **单调 TSO**：布局同构 TiDB（物理毫秒<<18 | 逻辑位），内存 CAS 发号 0 I/O，预留上界持久化，Leader 切换从持久化上界续发保证时间戳不回退。
- **SQL 引擎**：Lexer→Parser→逻辑优化(谓词下推/列裁剪)→物理计划→火山模型执行(含并行聚合)，支持 DDL/DML/聚合查询。
- **性能与可靠性（单机 3 节点实测，AMD 6800HS/HDD）**：
  - 安全档（`raft_sync=true`，金融场景配置）：写 TPS ≈ **____**、P99 ≈ **____ ms**；
  - 高吞吐档（`raft_sync=false`）：写 TPS ≈ **16,357**、P99 ≈ 5ms；
  - 混沌测试随机 kill Leader 2 次、秒级切换（<3s）、32 线程压测 **0 失败**。
- **技术栈**：C++17 · RocksDB · braft · brpc · Protobuf

---

## 相比原简历的关键改动 & 理由

| 原写法 | 问题 | 改为 |
|---|---|---|
| "某银行核心资金清算系统…迁移至自研数据库" | 把个人项目谎称真实生产迁移，一问即崩 | "面向金融信创场景的自研分布式事务数据库（个人项目）" |
| "TPS 从 3K 提升至 15K(5x)" | 拿自研 KV 比 Oracle，苹果比橘子 | 只写自测、带环境的数字，并区分安全档/高吞吐档 |
| "日均千万级交易 / 全年 99.99%" | 生产口径，个人项目给不出 | 删除，换成可复现的混沌测试结论 |
| "目标 500+ stars" | aspirational，不该进简历 | 删除 |
| "简化版 Percolator(Prewrite/Commit)" | 低估了自己 | 补全 Rollback/CheckTxnStatus/ResolveLock/TTL |

---

## ⚠️ 关于持久化的正确说法（面试会问，先想清楚）

**别把 `disableWAL=true` 和"不安全"划等号**——这点要讲对，否则反而露怯：

- RocksDB 层 `disableWAL=true` 是**合理的**：状态机的真正持久化来源是 **braft 的 Raft 日志**，宕机重启后状态机由"快照 + 重放 Raft 日志"重建，RocksDB 自己的 WAL 是冗余的。所以关它不丢数据。
- **真正决定"宕机丢不丢已提交数据"的开关是 `--raft_sync`**：
  - `raft_sync=true`：每次 AppendEntries 落盘 fsync，**多数派都持久化后才 commit** → 宕机不丢已提交数据 → **金融场景必须用这档**。
  - `raft_sync=false`：Raft 日志不 fsync，靠 page cache，宕机可能丢刚提交的日志 → 只适合跑基准/非关键数据。
- 你 README/perf_report 里冲 TPS 用的是 `raft_sync=false`，所以简历讲金融时**必须以安全档为准**，把高吞吐档作为"关闭 fsync 后的上限参考"。这样讲，"性能与数据安全的取舍"就成了你的加分点而不是破绽。

（注：`--raft_sync_meta=true` 已开，meta 始终同步落盘，无需改。）

---

## 📌 怎么跑出"安全档"数字（填上面的 ____）

文件桥接只能读写、不能在你机器上编译执行，所以这几步需要你在本机跑：

1. 改 `scripts/start_cluster.sh`：把 `--raft_sync=false` 改成 `--raft_sync=true`（其余参数不动）。
2. 编译并起集群：
   ```bash
   cd build && make -j$(nproc) kv_server perf_test
   ../scripts/start_cluster.sh && sleep 5
   grep "Became LEADER" /tmp/raftkv_820*.log   # 确认选主
   ```
3. 跑纯写基准（与高吞吐档同参数，只有 raft_sync 不同，保证可对比）：
   ```bash
   ./perf_test --mode=write --threads=32 --duration_s=60 \
               --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
               --key_range=50000
   ```
4. 记录输出的 TPS / avg / P99，填进上面的 ____。
5. （可选，更有说服力）用 `scripts/bench_rw_compare.sh` 直接跑 raft_sync 开/关对比，得到"安全档 vs 高吞吐档"两行数据，简历/面试都能用。

> 提示：HDD 上开 fsync 的 TPS 会明显低于关 fsync（perf_report 里初始 raft_sync=true 约 7k）；如果能在 **SSD/NVMe** 上再测一组，安全档数字会好看很多，也更接近金融真实部署，建议补测。
