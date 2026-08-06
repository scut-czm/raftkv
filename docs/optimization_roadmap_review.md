# RaftSQL 优化方向盘点 + A/B 深挖落地方案

> 目标：在已有基础上，主攻 **B（存储侧计算下推 / 类 Coprocessor）** 做技术深度锚点，
> 顺带 **A（EXPLAIN + 统计信息 + 简单 CBO）** 做可演示性与优化器故事。两者互补，直指"和 TiDB 的差距在哪、我怎么补"。

---

## 0. 已完成方向盘点（对照 A–F 路线图）

| 方向 | 状态 | 说明 |
|---|---|---|
| SQL 引擎 + RBO | ✅ 已做 | Lexer→Parser→逻辑优化(谓词下推/列裁剪)→物理计划→火山模型 + parallel_aggregate |
| MVCC + 事务（D） | ✅ 已做 | 三 CF 快照隔离 + Percolator 2PC + Rollback + CheckTxnStatus/ResolveLock + TTL + 单调 TSO |
| Raft 工程化（F 部分） | ✅ 已做 | on_apply 批量 WriteBatch、Leader Lease Read、Checkpoint 硬链接快照 |
| 性能/可靠性调优 | ✅ 已做 | raft_sync/append buffer/snapshot_interval + 混沌 failover |
| **A：EXPLAIN/统计/CBO** | ❌ 未做 | 本文档主攻之一 |
| **B：Coprocessor 下推** | ❌ 未做 | 本文档主攻之一（与 TiDB 最本质差距） |
| C 二级索引/TopN/JOIN/向量化 | ❌ 未做 | 后置 |
| E Multi-Raft/Region | ❌ 未做 | 天花板最高、工程量最大，作长期规划 |

---

## A. EXPLAIN + 统计信息 + 简单 CBO（先做，性价比最高）

**为什么先做 A**：工程量小、可演示性极强，且 A2 的统计信息是 B 的聚合/投影下推做代价估算的前置依赖。

### A1. EXPLAIN
- **做什么**：`EXPLAIN SELECT ...` 打印逻辑计划树 + 物理算子树（缩进树形），标注每个节点被哪条 RBO 规则改写。
- **落地步骤**：
  1. Parser 加 `EXPLAIN` 关键字，AST 加 `is_explain` 标记。
  2. `LogicalPlan` 各节点 + 每个 Operator 加 `ToString(int indent)`。
  3. `SQLExecutor` 遇到 explain 分支：只 build plan、格式化输出，不 `Open/Next/Close`。
- **完成标准**：`WHERE id=2` 能看到从 `Filter→Scan` 改写成 `Scan[range: users:00000002, users:00000003)`。

### A2. 统计信息
- **做什么**：每表 row count、每列 min/max（进阶：等宽直方图、NDV）。
- **落地步骤**：
  1. 存储于 `__stats__/{table}`，复用 SchemaManager 的缓存机制（和 `__schema__/` 同一套）。
  2. `ANALYZE TABLE t` 全量重建；INSERT/DELETE 增量更新 row count（min/max 可只在 ANALYZE 时算）。
  3. 统计信息随快照持久化（写 meta CF），启动/装快照后 reload。
- **完成标准**：`ANALYZE` 后 EXPLAIN 每个节点显示 `estRows`。

### A3. 简单 CBO：Get vs Scan / access path 选择
- **做什么**：物理计划阶段用统计信息估代价选择 access path。
- **规则（起步够用）**：
  - 主键等值 → 点查 `Get`；
  - 窄范围（估算命中行 < 阈值，如 total 的 5%）→ 窄 `Scan`；
  - 否则 → 全表 `Scan`；
  - （有二级索引后）比较"索引扫+回表"代价 vs "全表扫"代价。
- **完成标准**：同一 SQL 在不同数据分布（`ANALYZE` 后）选出不同物理计划，EXPLAIN 可见。

**A 的面试话术**：把"RBO（规则固定）升级成 CBO（按数据分布选计划）"讲成一条演进主线，能直接回答"优化器怎么选 access path""为什么同一条 SQL 计划会变"这类必问题。

---

## B. 存储侧计算下推 / 类 Coprocessor（主攻，技术深度锚点）

**现状痛点**：现在的"谓词下推"只缩小 scan key 范围；**非主键条件**要把整段数据拉回执行端再过滤，网络回传是最大浪费。目标是把过滤/投影/聚合"下沉"到存储节点，只回传有用数据。

### B1. 带条件 Scan（ScanWithFilter）
- **做什么**：RaftKV 新增 RPC `ScanWithFilter(start, end, filter_expr, limit)`，把序列化后的谓词发到存储节点，节点侧解码行、就地过滤，只回传命中行。
- **落地步骤**：
  1. proto 定义**表达式树**（复用 SQL 层 `Expr` → protobuf `ExprPB`：列引用/常量/比较/逻辑与或）。
  2. 存储侧实现一个 `ExprEvaluator`，输入解码后的 Row + schema，输出 bool。
  3. **关键**：`ExprEvaluator` 与执行端 `FilterOperator` **共用同一套求值代码**，避免"存储侧和执行端语义漂移"（例如 NULL 比较、类型转换规则不一致）。
  4. 读路径接线性一致：下推 Scan 走 leader lease read / ReadIndex + applied barrier（复用你现有机制）。
- **完成标准**：`SELECT name FROM users WHERE age>20`（age 非主键）网络回传量**随选择率线性下降**。

### B2. 投影下推
- **做什么**：filter 之外附带"需要的列列表"，存储侧回传裁剪后的行（只含所需列）。
- **落地**：`ScanWithFilter` 请求里加 `repeated column_id needed_cols`；存储侧解码后只重编码需要的列。
- **完成标准**：`SELECT name FROM ...` 回传行不含 age/其他列。

### B3. 聚合下推
- **做什么**：`COUNT/SUM/MIN/MAX` 在存储节点算**部分结果（partial）**，执行端做**最终合并（final）**。
- **落地**：
  1. 请求里带聚合描述（函数类型 + 目标列）；存储侧返回 partial（如 count 局部值、sum 局部和、min/max 局部极值）。
  2. 执行端复用你已有的 `parallel_aggregate` 雏形做 final merge——**这是你现成的对接点**。
- **完成标准**：聚合查询回传量恒定 **O(1)**（只回 partial，不回原始行）。

### B 的注意点（面试深挖点）
- **语义一致性**：下推求值器与执行端必须同源，否则同一 SQL 在"下推 vs 不下推"下结果不同——这是 Coprocessor 最容易踩的坑，能主动讲出来是加分。
- **线性一致**：下推执行在读路径完成，仍要走 ReadIndex/lease 保证读到已 committed 数据。
- **回退路径**：存储侧不支持的表达式（如复杂函数）要能优雅回退到"拉回执行端过滤"，不能因下推失败而报错。
- **量化故事**：这是最能打动面试官的点——用 EXPLAIN(来自 A) 展示"下推前后回传行数/字节数对比"，把"减少数据搬运"讲成可测的收益。

---

## 推进顺序与里程碑

1. **A1 EXPLAIN**（最快见效，后续所有优化都靠它可视化）。
2. **A2 统计信息 + A3 简单 CBO**（Get/Scan 选择，EXPLAIN 显示 estRows）。
3. **B1 带条件 Scan**（下推核心，先打通 filter 下推 + 回退路径）。
4. **B2 投影下推 → B3 聚合下推**（对接 parallel_aggregate）。
5. 用一组基准对比"下推前/后"回传字节数与延迟，写进 perf_report + 简历。

**串起来的一句话**：A 让优化"看得见、能按数据分布选计划"，B 让计算"下沉到数据所在处、只搬有用的数据"——这正是 RaftSQL 从"能跑的自研库"走向"懂分布式存算协同"的关键一跳，也是和 TiDB 差距收敛最快的方向。
