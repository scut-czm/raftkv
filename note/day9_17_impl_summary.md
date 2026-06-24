# RaftSQL Day 9–17 实现总结文档

> **说明**：本文档是实现框架与学习笔记模板。  
> - `- [ ]` 为待完成 / 待理解的 TODO 项  
> - `- [x]` 为已完成项  
> - 代码块为关键参考实现，可自行注释补充  

---

## 目录

- [Day 9 – SQL Lexer & Parser](#day-9--sql-lexer--parser)
- [Day 10 – Logical Planner & Optimizer](#day-10--logical-planner--optimizer)
- [Day 11 – Physical Planner & Operator Engine](#day-11--physical-planner--operator-engine)
- [Day 13 – Schema Manager (CREATE TABLE)](#day-13--schema-manager-create-table)
- [Day 14 – Row Codec & INSERT](#day-14--row-codec--insert)
- [Day 15 – SELECT & Predicate Pushdown](#day-15--select--predicate-pushdown)
- [Day 16 – UPDATE / DELETE / Aggregate](#day-16--update--delete--aggregate)
- [Day 17 – 集成适配器 & SQL Client](#day-17--集成适配器--sql-client)
- [Bug 修复记录](#bug-修复记录)

---

## Day 9 – SQL Lexer & Parser

### 文件
- `src/sql/lexer.h / lexer.cc`
- `src/sql/ast.h / ast.cc`
- `src/sql/parser.h / parser.cc`
- `tests/unit/parser_test.cc`

---

### `Lexer`

#### TODO List
- [ ] 理解 `TokenType` 枚举中每种 token 的含义
- [ ] 理解 `SkipWhitespace()` 如何处理注释
- [ ] `ScanIdentOrKeyword()` 如何区分关键字与标识符（`LookupKeyword`）
- [ ] `ScanNumber()` 是否支持浮点数？
- [ ] `ScanString()` 如何处理转义字符？
- [ ] `ScanOperator()` 对 `>=`、`!=` 等双字符运算符的处理

```cpp
// TODO: 在此填写你对 Lexer::NextToken() 的理解注释
```

---

### `Expr` AST 节点

#### TODO List
- [ ] 理解三种 `Kind`：`kColumn`、`kLiteral`、`kBinOp`
- [ ] `MakeBinOp` 如何构成 `AND`/`OR` 树
- [ ] `unique_ptr` 为何不能直接复制（影响 `SelectStmt` 的使用）

```cpp
// TODO: 画出 "age > 20 AND name = 'Alice'" 的 Expr 树结构
```

---

### `Parser::ParseCreate()`

#### TODO List
- [ ] 理解递归下降解析的入口分派逻辑
- [ ] `ParseCreate()` 支持哪些列类型？
- [ ] ~~Bug 已修复~~ 内联 `PRIMARY KEY` 语法（见 [Bug 修复记录](#bug-修复记录)）
- [ ] `ParseOrExpr` / `ParseAndExpr` / `ParseCmpExpr` 的优先级如何体现

```cpp
// TODO: 手写一遍 ParseSelect() 的核心逻辑，理解递归下降
```

---

## Day 10 – Logical Planner & Optimizer

### 文件
- `src/sql/logical_plan.h / logical_plan.cc`
- `src/sql/planner.h / planner.cc`
- `src/sql/logical_optimizer.h / logical_optimizer.cc`
- `tests/unit/optimizer_test.cc`

---

### `LogicalPlan` 节点类型

#### TODO List
- [ ] 画出 `SELECT name FROM users WHERE age > 20` 的完整逻辑计划树
- [ ] `kScan` / `kFilter` / `kProject` / `kAggregate` / `kLimit` 各节点存储哪些字段
- [ ] `kEmpty` 节点何时出现（常量折叠 `WHERE false`）

```
// TODO: 在此填写逻辑计划树示意图
SELECT name FROM users WHERE age > 20

  Project [name]
    └── Filter [age > 20]
          └── Scan [users]
```

---

### `Planner::PlanSelect()`

#### TODO List
- [ ] 聚合查询（`SELECT COUNT(*)`）如何在 Plan 中体现
- [ ] `PlanSelect` 为何需要改为 `public`（见 Bug 修复）
- [ ] `where_expr` 含 `unique_ptr` 为何不能做 `Stmt` 的值复制

```cpp
// TODO: 描述 Planner 将 SelectStmt → LogicalPlan 的步骤
```

---

### `LogicalOptimizer` 三大规则

#### TODO List
- [ ] **谓词下推（PredicatePushdown）**：Filter 谓词如何移入 Scan
- [ ] **列裁剪（ColumnPruning）**：如何减少不必要的列传递
- [ ] **常量折叠（ConstantFolding）**：`1 = 1` → `true`，`1 = 2` → `kEmpty`

```cpp
// TODO: 对谓词下推写一段注释，说明它如何降低 I/O
```

---

## Day 11 – Physical Planner & Operator Engine

### 文件
- `src/sql/operator.h`
- `src/sql/operators/table_scan_operator.h/cc`
- `src/sql/operators/filter_operator.h/cc`
- `src/sql/operators/project_operator.h/cc`
- `src/sql/operators/aggregate_operator.h/cc`
- `src/sql/operators/limit_operator.h/cc`
- `src/sql/physical_planner.h/cc`

---

### Volcano 执行模型

#### TODO List
- [ ] 理解 `Open()` / `Next()` / `Close()` 三阶段语义
- [ ] `Next()` 返回 `false` 意味着什么
- [ ] Volcano 模型的优点与缺点（与向量化执行对比）

```
// TODO: 画出 Select-Filter-Scan 的算子调用链
Executor
  └── calls root_op->Next()
        └── ProjectOperator::Next()
              └── FilterOperator::Next()
                    └── TableScanOperator::Next()
```

---

### `TableScanOperator::Open()` / `Next()`

#### TODO List
- [ ] `CanNarrowScanRange()` 何时返回 true（主键等值/范围谓词）
- [ ] `NarrowScanRange()` 对 `=`、`>`、`<`、`>=`、`<=` 如何生成 key 范围
- [ ] ~~Bug 已修复~~ 主键值 vs row_id 对齐问题（见 Bug 修复记录）
- [ ] 非主键列谓词在 `Next()` 中如何过滤

```cpp
// TODO: 解释为何 WHERE id=2 的下推 key 必须与 INSERT 的存储 key 一致
```

---

### `PhysicalPlanner::Plan()`

#### TODO List
- [ ] `PlanType` 到具体算子的映射关系
- [ ] `SchemaProvider` 函数类型的作用（延迟绑定 schema）
- [ ] 为何 `PhysicalPlanner` 不直接持有 `SchemaManager`

```cpp
// TODO: 列出 PhysicalPlanner 对每种 PlanType 创建的算子类型
// kScan      → TableScanOperator
// kFilter    → FilterOperator
// kProject   → ProjectOperator
// kAggregate → AggregateOperator
// kLimit     → LimitOperator
```

---

## Day 13 – Schema Manager (CREATE TABLE)

### 文件
- `proto/schema.proto`
- `src/sql/kv_client_interface.h`
- `src/sql/schema_manager.h/cc`
- `tests/unit/mock_kv_client.h`
- `tests/unit/schema_manager_test.cc`

---

### `schema.proto` 设计

#### TODO List
- [ ] `DataType` 枚举支持哪些类型
- [ ] `ColumnDef`（proto）与 `ColumnSpec`（AST）的区别（见 Bug 修复）
- [ ] `TableSchema.next_row_id` 字段的用途

```protobuf
// TODO: 在此填写你对 schema.proto 的理解
```

---

### `SchemaManager::CreateTable()` / `GetSchema()`

#### TODO List
- [ ] Schema 在 KV 中的 key 格式：`__schema__/{table_name}`
- [ ] 缓存（`cache_`）的作用：避免重复序列化反序列化
- [ ] `InvalidateCache()` 何时需要调用
- [ ] `ParseFromString` 失败时如何处理

```cpp
// TODO: 写出 CreateTable 到 KV Store 的完整调用链
```

---

### `KvClientInterface` 抽象设计

#### TODO List
- [ ] 为什么要抽象 `KvClientInterface` 而非直接用 `KVClient`
- [ ] `Scan(start, end, limit)` 的语义：左闭右开区间
- [ ] `BatchPut` 的原子性保证（在 RaftKV 中是否原子）

---

## Day 14 – Row Codec & INSERT

### 文件
- `proto/row.proto`
- `src/sql/row_codec.h/cc`
- `src/sql/row_id_allocator.h/cc`
- `tests/unit/row_codec_test.cc`
- `tests/unit/insert_test.cc`

---

### Row Key 格式

#### TODO List
- [ ] Key 格式：`{table_name}:{row_id:08d}` 为何用 8 位补零
- [ ] ~~Bug 已修复~~ row_id 用主键列值而非分配器值（见 Bug 修复）
- [ ] `TableScanRange` 用 `;`（ASCII 59）作为结束 key 的原理

```
// TODO: 写出三行数据的 key 示例
users:00000001  → id=1, Alice
users:00000002  → id=2, Bob
users:00000003  → id=3, Charlie
```

---

### `RowCodec::EncodeRow()` / `DecodeRow()`

#### TODO List
- [ ] `Value` proto 的 `oneof` 如何处理不同类型（INT/VARCHAR/FLOAT/BOOL）
- [ ] NULL 值如何编码（`is_null = true`）
- [ ] `ValueToString` 中 float 精度问题

```cpp
// TODO: 画出一行数据 {id=1, name="Alice", age=25} 的 Protobuf 序列化结构
```

---

### `RowIdAllocator` 分段预分配

#### TODO List
- [ ] 分段预分配的思想来源（TBase / Oracle SCN）
- [ ] `kSegmentSize = 1000` 的含义：每 1000 次分配才做一次 Raft 写
- [ ] `Init(1)` 时 `current_=1, limit_=1`，第一次 `Allocate()` 流程
- [ ] ~~已知问题~~ 分配器首次返回 2 而非 1（已被主键值覆盖修复）

```cpp
// TODO: 画出 Init(1) 后第 1、2、3 次 Allocate() 的状态变化
```

---

## Day 15 – SELECT & Predicate Pushdown

### 文件
- `src/sql/sql_executor.cc` (`ExecuteSelect`)
- `tests/unit/select_test.cc`

---

### `SQLExecutor::ExecuteSelect()`

#### TODO List
- [ ] 聚合路径（`!stmt.agg_func.empty()`）与普通 SELECT 路径的分叉点
- [ ] 普通 SELECT 走 `Planner→Optimizer→PhysicalPlanner→Execute` 完整流水线
- [ ] 为何聚合路径单独实现（不走 PhysicalPlanner）
- [ ] `root_op->Open()` → `Next()` 循环 → `Close()` 的执行模式

```cpp
// TODO: 描述 SELECT * FROM users WHERE age > 20 的完整执行路径
// 1. Parser 解析 → SelectStmt
// 2. Planner::PlanSelect → LogicalPlan
// 3. Optimizer → 谓词下推
// 4. PhysicalPlanner → Operator 树
// 5. Execute: Open → Next* → Close
```

---

## Day 16 – UPDATE / DELETE / Aggregate

### 文件
- `src/sql/sql_executor.cc` (`ExecuteUpdate`, `ExecuteDelete`)
- `src/sql/operators/aggregate_operator.h/cc`
- `tests/unit/update_delete_test.cc`
- `tests/unit/aggregate_test.cc`

---

### `ExecuteUpdate()` / `ExecuteDelete()`

#### TODO List
- [ ] 为何 UPDATE/DELETE 不走 PhysicalPlanner（直接 Scan + 过滤）
- [ ] UPDATE 的 in-place 更新：相同 key，新 value
- [ ] DELETE 的实现：`kv_client_->Delete(key)`
- [ ] `set_clauses` 如何从 parser 传递到 executor

```cpp
// TODO: 写出 UPDATE users SET age=99 WHERE id=1 的执行步骤
```

---

### `AggregateOperator`

#### TODO List
- [ ] `Open()` 时一次性消费所有子算子行（blocking 算子）
- [ ] 支持的函数：`COUNT` / `SUM` / `MIN` / `MAX`
- [ ] `COUNT(*)` vs `COUNT(col)` 的区别
- [ ] `Next()` 只返回一行结果（聚合结果）

```cpp
// TODO: 为何 AggregateOperator 是 blocking 的？与 Filter 的流式处理对比
```

---

## Day 17 – 集成适配器 & SQL Client

### 文件
- `src/sql/raft_kv_client_adapter.h/cc`
- `client/sql_client_main.cc`
- `tests/integration/sql_end_to_end_test.cc`

---

### `RaftKvClientAdapter`

#### TODO List
- [ ] 适配器模式（Adapter Pattern）的作用
- [ ] `raftkv::KVClient::Get` 返回 `bool` + out 参数，适配为返回空串表示不存在
- [ ] `Scan` 的 `limit` 参数默认值 10000 是否足够

```cpp
// TODO: 写出适配器的接口转换关系
// KvClientInterface::Get(key) → string
// raftkv::KVClient::Get(key, *value, *found) → bool
```

---

### `sql_client` 交互式 Shell

#### TODO List
- [ ] 多行 SQL 如何拼接（以 `;` 为结束符）
- [ ] `--peers` flag 如何指定集群地址
- [ ] 错误处理：解析失败 vs 执行失败的不同展示

```bash
# TODO: 填写启动 sql_client 的示例命令
./sql_client --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202

raftsql> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64));
OK
raftsql> INSERT INTO users (id, name) VALUES (1, 'Alice');
OK (1 row affected)
raftsql> SELECT * FROM users;
id | name
----------------------------------------
1  | Alice
(1 row)
```

---

### `sql_end_to_end_test`

#### TODO List
- [ ] 集成测试需要真实 3 节点 RaftKV 集群运行
- [ ] 启动命令（参考 Day 3 的集群启动脚本）
- [ ] `FaultToleranceLeaderFailover` 测试如何验证容错

```bash
# TODO: 填写运行集成测试的命令
./sql_end_to_end_test --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202
```

---

## Bug 修复记录

> 本节记录 Day 9–17 实现过程中遇到并修复的所有 Bug。

---

### Bug 1：`ColumnDef` 名称冲突（命名空间污染）

**文件**：`src/sql/ast.h`

**现象**：
```
error: no declaration matches 'void raftsql::ColumnDef::set_name()'
```

**根因**：  
AST 中定义了 `struct ColumnDef`，Protobuf 生成的 `schema.pb.h` 也在 `raftsql` 命名空间下生成了 `class ColumnDef`。两者同名，编译器将 AST 结构体与 proto 类混淆，导致 proto 生成方法（`set_name()`、`set_type()` 等）找不到。

**修复**：  
将 AST 中的 `ColumnDef` 重命名为 `ColumnSpec`，更新所有引用（`ast.h`、`parser.cc`）。

```cpp
// 修复前
struct ColumnDef { std::string name; std::string type; bool primary_key; };

// 修复后
struct ColumnSpec { std::string name; std::string type; bool primary_key; };
```

**教训**：与 Protobuf 共享命名空间时，须避免与 proto message 同名的 C++ 类型。

---

### Bug 2：`SELECT * FROM users WHERE id = N` 返回错误行

**文件**：`src/sql/sql_executor.cc`（`ExecuteInsert`）

**现象**：
```
// WHERE id = 2 时返回 Alice（id=1），而非 Bob（id=2）
EXPECT_EQ(r.rows[0].at("name"), "Bob");  // 实际是 "Alice"
```

**根因**：  
`RowIdAllocator` 从 `Init(1)` 开始，但 `Allocate()` 因为 `fetch_add` 后检查 `id < limit` 的边界问题，**第一次返回 2**（而非 1）。

于是 INSERT 的存储 key：
```
INSERT id=1, Alice → key = "users:00000002"  ← 分配器返回 2
INSERT id=2, Bob   → key = "users:00000003"
```

但 `TableScanOperator::NarrowScanRange()` 使用 SQL 中的主键列值生成 key：
```
WHERE id = 2 → 计算 key = "users:00000002"  ← 指向 Alice！
```

**修复**：  
`ExecuteInsert` 时，若存在整数主键列，直接用主键列的值作为 row_id，不使用分配器值：

```cpp
// 修复后
const std::string pk_col = RowCodec::GetPrimaryKeyColumn(schema);
if (!pk_col.empty()) {
  auto pk_it = row.find(pk_col);
  if (pk_it != row.end()) {
    try { row_id = std::stoll(pk_it->second); } catch (...) {}
  }
}
auto key = RowCodec::EncodeRowKey(stmt.table_name, row_id);
```

**教训**：存储 key 的生成逻辑必须与谓词下推的 key 计算逻辑完全一致，否则点查会命中错误行。

---

### Bug 3：Parser 不支持内联 `PRIMARY KEY` 语法

**文件**：`src/sql/parser.cc`（`ParseCreate()`）

**现象**：
```
Parse error: Expected token type 21 but got 'PRIMARY'
```
执行 SQL：`CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64))`

**根因**：  
Parser 的 `ParseCreate()` 在列定义循环中只处理了两种情况：
1. `PRIMARY KEY(col)` — 作为独立子句出现在列列表中
2. `name type` — 普通列定义

对于 `id INT PRIMARY KEY` 这种**内联于列定义末尾**的语法，在推入列后直接调用 `Match(kComma)`。但下一个 token 是 `PRIMARY`（不是逗号），导致 break 出循环，然后 `Expect(kRParen)` 遇到 `PRIMARY` 报错。

**修复**：  
在解析完列类型后，检测 `PRIMARY KEY` token：

```cpp
// 在 col.type 赋值之后、push_back 之前添加：
if (Peek().type == TokenType::kPrimary) {
  Advance();
  Expect(TokenType::kKey);
  col.primary_key = true;
  if (has_error_) return std::nullopt;
}
stmt.columns.push_back(std::move(col));
```

**教训**：SQL 标准允许列约束写在列类型后面（inline constraint），Parser 需要同时支持 inline 和 table-level 两种写法。

---

### Bug 4：`SelectStmt` 含 `unique_ptr` 无法复制为 `Stmt` variant

**文件**：`src/sql/sql_executor.cc`（`ExecuteSelect`）

**现象**：
```
error: conversion from 'const raftsql::SelectStmt' to non-scalar type
       'raftsql::Stmt' requested
```

**根因**：  
`SelectStmt` 包含 `std::unique_ptr<Expr> where_expr`，无复制构造函数。`Stmt` 是 `std::variant<SelectStmt, ...>`，从 `const SelectStmt&` 构造 `Stmt` 需要复制，但 `SelectStmt` 不可复制。

```cpp
// 错误写法
Stmt ast_stmt = stmt;  // ← 试图复制含 unique_ptr 的 SelectStmt
auto logical_plan = planner.Plan(ast_stmt);
```

**修复**：  
将 `Planner::PlanSelect()` 改为 `public`，直接传入 `const SelectStmt&` 绕过 variant 包装：

```cpp
// planner.h 中将 PlanSelect 移至 public
std::unique_ptr<LogicalPlan> PlanSelect(const SelectStmt& stmt);

// sql_executor.cc 中直接调用
auto logical_plan = planner.PlanSelect(stmt);
```

**教训**：含 `unique_ptr` 的结构体只能移动不能复制。在接口设计时，应避免强制将 move-only 类型包装进 variant 再复制传递。

---

*文档生成时间：Day 17 完成后*  
*测试结果：9/9 SQL 单元测试全部通过*
