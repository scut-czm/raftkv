# 开发指南

## 环境准备

### 依赖安装（Ubuntu 22.04 / 24.04）

```bash
# 基础工具
sudo apt-get update
sudo apt-get install -y build-essential cmake git

# 核心依赖
sudo apt-get install -y \
  libgflags-dev \
  libprotobuf-dev protobuf-compiler \
  libgtest-dev \
  libsnappy-dev \
  zlib1g-dev \
  liblz4-dev \
  libssl-dev

# RocksDB（推荐 v9.0.0+）
sudo apt-get install -y librocksdb-dev
# 或从源码编译：
# git clone https://github.com/facebook/rocksdb && cd rocksdb
# make shared_lib -j$(nproc) && sudo make install-shared

# brpc
# git clone https://github.com/apache/brpc && cd brpc
# mkdir build && cd build && cmake .. && make -j$(nproc) && sudo make install

# braft（作为子项目引入，无需单独安装）
# 项目通过 add_subdirectory(../braft_learning/braft) 引入
```

### 目录结构假设

本项目依赖同级目录下的 `braft_learning/braft`：

```
database_learning/
├── raftkv/          ← 本项目
└── braft_learning/
    └── braft/       ← braft 源码（子项目）
```

---

## 编译

```bash
cd raftkv
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
```

**编译模式说明**：

| 模式              | 说明                             | 适用场景   |
| ----------------- | -------------------------------- | ---------- |
| `RelWithDebInfo`  | `-O2` + 调试符号（默认）         | 性能测试   |
| `Debug`           | `-O0` + 完整调试信息             | 开发调试   |
| `Release`         | `-O3`，无调试符号                | 生产部署   |

---

## 运行测试

### 单元测试

```bash
cd build

# RocksDB 存储层单测
./storage_test

# braft 状态机单测（快照保存/恢复）
./state_machine_test

# 客户端 SDK 单测（redirect 逻辑，重试机制）
./client_test
```

### 集成测试（需启动集群）

```bash
# 启动 3 节点集群
../scripts/start_cluster.sh
sleep 5

# 验证 Leader 选出
grep "Became LEADER" /tmp/raftkv_820*.log

# 基础功能验证
./kv_client --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
            --command=put --key=hello --value=world
./kv_client --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
            --command=get --key=hello --linearizable=true

# 停止集群
../scripts/stop_cluster.sh
```

### 性能基准测试

```bash
../scripts/start_cluster.sh && sleep 5

# 纯写基准（32 线程，1KB value，60s）
./perf_test --mode=write --threads=32 --duration_s=60 \
            --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
            --key_range=50000

# 7:3 读写混合（16 线程）
./perf_test --mode=mixed --threads=16 --duration_s=60 \
            --write_ratio=0.7 \
            --peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202

../scripts/stop_cluster.sh
```

### 混沌测试

```bash
# Leader 随机 Kill + 自动重启（验证高可用）
bash tests/chaos/failover_test.sh

# 72h 长稳压测
bash tests/chaos/long_stress_test.sh
```

---

## 代码规范

遵循 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)。

### 命名规范速查

| 类型              | 规范                     | 示例                       |
| ----------------- | ------------------------ | -------------------------- |
| 类 / 结构体       | `PascalCase`             | `KvClient`, `RocksDbStorage` |
| 函数              | `PascalCase`             | `CreateCheckpoint()`       |
| 局部变量 / 参数   | `snake_case`             | `start_key`, `value_size`  |
| 类成员变量        | `snake_case_`（尾部下划线）| `storage_`, `is_leader_`  |
| 常量              | `kPascalCase`            | `kMaxRetry`, `kDefaultTimeout` |
| 命名空间          | `snake_case`             | `namespace raftkv`         |

### 格式要求

- 缩进：**2 空格**，禁止 Tab
- 行宽：**80 字符**
- 头文件保护：`#pragma once` 或 `RAFTKV_PATH_FILE_H_` 格式的 Include Guard
- 提交前运行 `clang-format`：

```bash
find src/ -name "*.cc" -o -name "*.h" | xargs clang-format -i
```

---

## 目录结构

```
src/
├── storage/     # RocksDB 封装（不依赖 braft，可单独测试）
├── raft/        # braft 状态机（依赖 storage/）
├── service/     # RPC 服务层（依赖 raft/）
└── client/      # 客户端 SDK（不依赖服务端，仅 brpc）
```

**依赖方向**：`service → raft → storage`，`client` 独立。
修改 `storage/` 只需跑 `storage_test`；修改 `raft/` 需跑 `state_machine_test`。

---

## 提交规范

每个 commit 对应一个完整功能，message 格式：

```
<type>(<scope>): <subject>

type: feat | fix | perf | test | docs | refactor
scope: storage | raft | service | client | test | scripts
```

示例：

```
feat(raft): implement Leader Lease Read in KVServiceImpl
fix(storage): initialize write_opts_.disableWAL in constructor
perf(raft): increase raft_apply_batch to 64 for higher throughput
test(chaos): add long_stress_test.sh for 72h stability test
docs: add ARCHITECTURE.md and CONTRIBUTING.md
```

### 项目各阶段 commit 对应关系

```
Day 1: feat(storage): RocksDB Storage + 单元测试
Day 2: feat(raft): KVStateMachine + 3 节点集群
Day 3: feat(service): ReadIndex + 客户端 SDK
Day 4: test: 单元测试 + 集成测试
Day 5: perf: 性能基准测试 + RocksDB 调优
Day 6: test(chaos): 混沌测试 + 稳定性修复
Day 7: docs: README + ARCHITECTURE + CONTRIBUTING
```

---

## 运维脚本说明

| 脚本                       | 用途                               |
| -------------------------- | ---------------------------------- |
| `scripts/start_cluster.sh` | 一键启动 3 节点集群（端口 8200-8202）|
| `scripts/stop_cluster.sh`  | 停止所有 kv_server 进程            |
| `scripts/find_leader.sh`   | 从日志中定位当前 Leader 节点       |
| `scripts/bench_compare.sh` | 多场景性能对比基准脚本             |
| `scripts/bench_rw_compare.sh` | 读写比例对比基准脚本            |

**节点日志位置**：`/tmp/raftkv_<port>.log`  
**节点数据目录**：`/tmp/raftkv_data_<port>/`

清理测试数据：

```bash
rm -rf /tmp/raftkv_data_820{0,1,2}
```
