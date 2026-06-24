# RaftKV

基于 **Raft 共识协议** + **RocksDB 存储引擎** 的分布式 KV 存储系统。

底层使用 [braft](https://github.com/baidu/braft) 实现 Raft 共识，
[RocksDB](https://github.com/facebook/rocksdb) 作为状态机持久化后端，
支持线性一致读（Leader Lease Read）、自动 Leader 重定向、毫秒级快照。

---

## 核心特性

- **强一致性**：写操作经 Raft 多数派共识，读操作支持 Leader Lease Read 线性一致读
- **高性能**：3 节点集群纯写 TPS = **16,357**（32 线程），混沌压测 TPS = **19,488**（[性能报告](docs/perf_report.md)）
- **高可用**：Leader 故障自动切换 < 3s，混沌测试 2 次随机 Kill + 32 线程压测 fails = 0
- **毫秒级快照**：基于 RocksDB Checkpoint（硬链接），快照期间不阻塞任何写入
- **完整 API**：Put / Get / Delete / Scan，客户端 SDK 自动重定向至 Leader

---

## 架构

```
┌──────────────────────────────────────────────────┐
│                   Client SDK                      │
│         (自动 redirect + 重试 + 线性一致读)        │
└────────────────────┬─────────────────────────────┘
                     │ brpc RPC
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
┌─────────┐   ┌─────────┐   ┌─────────┐
│ Node 0  │   │ Node 1  │   │ Node 2  │
│ :8200   │◄──│ :8201   │◄──│ :8202   │
│ (Leader)│──►│(Follower│──►│(Follower│
├─────────┤   ├─────────┤   ├─────────┤
│KVService│   │KVService│   │KVService│
│ (brpc)  │   │ (brpc)  │   │ (brpc)  │
├─────────┤   ├─────────┤   ├─────────┤
│  braft  │   │  braft  │   │  braft  │
│ (Raft)  │   │ (Raft)  │   │ (Raft)  │
├─────────┤   ├─────────┤   ├─────────┤
│KVState  │   │KVState  │   │KVState  │
│Machine  │   │Machine  │   │Machine  │
├─────────┤   ├─────────┤   ├─────────┤
│ RocksDB │   │ RocksDB │   │ RocksDB │
│data/ CF │   │data/ CF │   │data/ CF │
└─────────┘   └─────────┘   └─────────┘
```

> 详细架构说明、数据流图、快照机制见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

---

## 快速开始

### 依赖

| 依赖                       | 说明                        |
| -------------------------- | --------------------------- |
| GCC >= 7 / Clang >= 5      | C++17                       |
| CMake >= 2.8.12            | 构建系统                    |
| RocksDB v9.0.0+            | 存储引擎                    |
| brpc                       | RPC 框架                    |
| braft                      | Raft 实现（作为子项目引入） |
| Protobuf                   | 序列化                      |
| gflags、gtest              | 命令行参数 + 测试框架       |
| snappy、zlib、lz4、openssl | 压缩 + 加密                 |

### 编译

```bash
git clone https://github.com/scut-czm/raftkv
cd raftkv && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
```

编译产物：

| 目标                 | 说明                |
| -------------------- | ------------------- |
| `kv_server`          | 节点服务端          |
| `kv_client`          | 命令行客户端        |
| `storage_test`       | Storage 单元测试    |
| `state_machine_test` | 状态机单元测试      |
| `client_test`        | 客户端 SDK 单元测试 |
| `perf_test`          | 性能基准测试工具    |

### 启动 3 节点集群

```bash
./scripts/start_cluster.sh
# 等待 5s 选举完成
sleep 5
grep "Became LEADER" /tmp/raftkv_820*.log
```

集群配置：`127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202`，数据目录 `/tmp/raftkv_data_<port>/`，日志 `/tmp/raftkv_<port>.log`。

### 基本操作

```bash
PEERS="--peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

# Put
./build/kv_client $PEERS --command=put --key=hello --value=world

# Get（线性一致读）
./build/kv_client $PEERS --command=get --key=hello --linearizable=true

# Scan（范围扫描）
./build/kv_client $PEERS --command=scan --start_key=a --end_key=z --limit=100

# Delete
./build/kv_client $PEERS --command=delete --key=hello
```

### 停止集群

```bash
./scripts/stop_cluster.sh
```

---

## API

| RPC                                     | 说明       | 一致性                                        |
| --------------------------------------- | ---------- | --------------------------------------------- |
| `Put(key, value)`                       | 写入键值对 | 强一致（Raft 共识）                           |
| `Get(key, linearizable)`                | 读取键值   | `false`=直读状态机 / `true`=Leader Lease Read |
| `Delete(key)`                           | 删除键     | 强一致（Raft 共识）                           |
| `Scan(start, end, limit, linearizable)` | 范围扫描   | `false`=直读 / `true`=Leader Lease Read       |

所有写操作若打到非 Leader 节点，响应中携带 `redirect` 字段，客户端 SDK 自动切换并重试。

**Proto 定义**：[proto/kv.proto](proto/kv.proto)

---

## 项目结构

```
raftkv/
├── proto/                    # Protobuf 定义（kv.proto）
├── src/
│   ├── storage/              # RocksDB 封装（data CF + meta CF）
│   │   ├── rocksdb_storage.h
│   │   └── rocksdb_storage.cc
│   ├── raft/                 # braft 状态机（on_apply / on_snapshot_save/load）
│   │   ├── kv_state_machine.h
│   │   └── kv_state_machine.cc
│   ├── service/              # RPC 服务层（Put/Get/Delete/Scan + redirect）
│   │   ├── kv_service.h
│   │   └── kv_service.cc
│   └── client/               # 客户端 SDK（自动 redirect + 重试）
│       ├── kv_client.h
│       └── kv_client.cc
├── server/                   # 服务端入口（server_main.cc）
├── client/                   # 命令行客户端（client_main.cc）
├── tests/
│   ├── unit/                 # 单元测试（storage / state_machine / client）
│   ├── integration/          # 集成测试 + 性能基准（perf_test）
│   └── chaos/                # 混沌测试（failover / long_stress）
├── scripts/                  # 运维脚本（start/stop/bench/find_leader）
├── docs/                     # 文档
└── CMakeLists.txt
```

---

## 性能

> 测试环境：Ubuntu 24.04.3，AMD Ryzen 7 6800HS（8c/16t），7.7 GiB RAM，75G HDD，单机 3 节点。  
> 详细数据与调优路径见 [docs/perf_report.md](docs/perf_report.md)。

### 基准测试结果

| 场景                   | 线程数 | TPS                 | avg 延迟        | P99 延迟              | 错误率 |
| ---------------------- | ------ | ------------------- | --------------- | --------------------- | ------ |
| 纯写（1KB value）      | 32     | **16,357**          | 1.95ms          | ~5ms                  | 0%     |
| 纯写（1KB value）      | 16     | 10,288              | 1.55ms          | 5.18ms                | 0%     |
| 7:3 读写混合           | 16     | 写 7,522 / 读 3,227 | 1.55ms / 1.32ms | 写 5.42ms / 读 9.04ms | 0%     |
| 批量 Scan（range=100） | 4      | 3,927               | 1.01ms          | 1.81ms                | 0%     |

### 混沌压测（Day 6）

| 指标                    | 数值                            |
| ----------------------- | ------------------------------- |
| TPS（32 线程，含 Kill） | **19,488**                      |
| fails                   | **0**                           |
| 随机 Kill 次数          | 2 次                            |
| Leader 切换恢复时间     | < 3s                            |
| 磁盘状态                | 稳定（snapshot_interval_s=120） |

### 关键调优配置

```bash
--raft_sync=false                    # 关闭 Raft log fsync，TPS +132%
--raft_max_append_buffer_size=4194304 # 4MB 批次，减少系统调用
--raft_enable_leader_lease=true      # Lease Read，读 P99 从 28ms → 1.9ms
--raft_apply_batch=64                # on_apply 批量提交
--snapshot_interval_s=120            # 定期快照，防止磁盘写满
# RocksDB: disableWAL=true, write_buffer_size=64MB, block_cache=256MB
```

---

## 测试

```bash
cd build

# 单元测试
./storage_test
./state_machine_test
./client_test

# 性能基准测试（需先启动集群）
../scripts/start_cluster.sh && sleep 5
./perf_test --mode=write --threads=32 --duration_s=60 \
            --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
            --key_range=50000

# 混沌测试（含随机 Kill）
bash ../tests/chaos/failover_test.sh

# 72h 长稳压测
bash ../tests/chaos/long_stress_test.sh
```

---

## 技术栈

| 组件         | 选型                    | 版本             |
| ------------ | ----------------------- | ---------------- |
| **共识协议** | braft（百度 Raft 实现） | master (ab0017f) |
| **RPC 框架** | brpc                    | 最新稳定版       |
| **存储引擎** | RocksDB（LSM-Tree）     | v9.0.0           |
| **序列化**   | Protobuf                | 系统版本         |
| **构建系统** | CMake                   | >= 2.8.12        |
| **测试框架** | Google Test             | 系统版本         |
| **语言标准** | C++17                   | GCC >= 7         |

---

## SQL 支持（RaftSQL）

在 RaftKV 之上构建了完整的 SQL 引擎层：

### 支持的 SQL

```sql
-- DDL
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT);

-- DML
INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25);
SELECT name, age FROM users WHERE age >= 18;
UPDATE users SET age = 26 WHERE name = 'Alice';
DELETE FROM users WHERE age < 18;

-- 聚合
SELECT COUNT(*) FROM users;
SELECT SUM(age) FROM users WHERE dept = 'Engineering';
SELECT MIN(age) FROM users;
SELECT MAX(age) FROM users;

## License

MIT
