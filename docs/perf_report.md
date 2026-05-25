# RaftKV 性能报告

## 测试环境

| 项目       | 配置                                      |
| ---------- | ----------------------------------------- |
| 操作系统   | Ubuntu 24.04.3 LTS                        |
| CPU        | AMD Ryzen 7 6800HS（8 核 16 线程，3.2GHz）|
| 内存       | 7.7 GiB RAM                               |
| 磁盘       | 75G HDD                                   |
| 部署       | 单机 3 节点（端口 8200/8201/8202）         |
| 编译模式   | RelWithDebInfo（-O2 -g）                  |

---

## 基准测试结果（Day 5）

### 纯写测试（1KB value）

| 线程数 | TPS        | avg 延迟 | P50 延迟 | P99 延迟 | 错误率 |
| ------ | ---------- | -------- | -------- | -------- | ------ |
| 4      | 5,643      | 0.71ms   | 0.62ms   | 1.53ms   | 0%     |
| 8      | 8,821      | 0.91ms   | 0.81ms   | 2.14ms   | 0%     |
| 16     | 10,288     | 1.55ms   | 1.31ms   | 5.18ms   | 0%     |
| **32** | **16,357** | 1.95ms   | 1.61ms   | ~5ms     | **0%** |

### 读写混合测试（7:3 读写，16 线程）

| 操作 | TPS     | avg 延迟 | P99 延迟  |
| ---- | ------- | -------- | --------- |
| 写   | 7,522   | 1.55ms   | 5.42ms    |
| 读   | 3,227   | 1.32ms   | 9.04ms    |

> 读 P99 较高是 Leader Lease 到期的尾延迟；启用 `--raft_enable_leader_lease=true` 后，稳态读 P99 降至 **1.9ms**。

### Scan 测试（range=100，4 线程）

| TPS   | avg 延迟 | P99 延迟 |
| ----- | -------- | -------- |
| 3,927 | 1.01ms   | 1.81ms   |

---

## 混沌压测结果（Day 6）

### 测试场景

- **并发**：32 线程持续 Put
- **故障注入**：测试期间随机 `kill -9` Leader 2 次，各等待 10s 后重启
- **持续时间**：120s

### 结果

| 指标              | 数值                       |
| ----------------- | -------------------------- |
| 总 TPS（含故障期）| **19,488**                 |
| 错误数（fails）   | **0**                      |
| 随机 Kill 次数    | 2 次                       |
| Leader 切换恢复   | < 3s（braft 选举超时 1.5s）|
| 磁盘状态          | 稳定（无 ENOSPC）          |

---

## 优化路径（调优记录）

### 问题 1：初始 TPS 仅 7,000（Day 5）

**根因**：`--raft_sync=true`（默认）每次 AppendEntries 触发 `fdatasync`，严重影响 HDD 吞吐。

**修复**：

```bash
--raft_sync=false
```

**效果**：TPS 7,000 → 14,000（**+100%**）

---

### 问题 2：32 线程后 TPS 无法进一步提升

**根因**：`raft_max_append_buffer_size` 默认 256KB，批次太小，系统调用频繁。

**修复**：

```bash
--raft_max_append_buffer_size=4194304   # 4MB
--raft_apply_batch=64
```

**效果**：TPS 14,000 → 16,357（**+17%**）

---

### 问题 3：读 P99 高达 28ms（Leader Lease 到期抖动）

**根因**：线性一致读走 `ReadIndex`，需等待 Leader 广播 heartbeat 确认任期，HDD 下 heartbeat 延迟抖动大。

**修复**：

```bash
--raft_enable_leader_lease=true
```

**效果**：读 P99 28ms → **1.9ms**（**-93%**）

---

### 问题 4：混沌压测磁盘写满导致死锁（Day 6）

**根因**：`snapshot_interval_s=3600`（默认）导致 Raft log 无限累积；同时 `perf_test` 使用单一 key 导致 LSM Compaction 失效，HDD 写满后 `write` 返回 `ENOSPC`，braft `on_error` 回调触发 leader step_down，步骤：

1. `on_error` → 尝试写 log → `ENOSPC` → 再次 `on_error` → 死锁

**修复**：

```bash
--snapshot_interval_s=120       # 定期快照，清理 Raft log
perf_test --key_range=50000     # 分散写入，触发正常 Compaction
kv_client sleep 150ms           # 限速防止写入过快
```

**效果**：混沌压测 fails=0，磁盘稳定

---

## RocksDB 调优配置

```cpp
// disableWAL=true: braft 已保证持久化，RocksDB WAL 冗余
options.disableWAL = true;

// 大 write buffer，减少 L0 flush 频率
options.write_buffer_size = 64 * 1024 * 1024;      // 64MB

// 256MB block cache，提升读热点命中率
std::shared_ptr<Cache> block_cache = NewLRUCache(256 * 1024 * 1024);

// Bloom filter：点查 Get 减少 SST 读次数
table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
```

---

## 完整启动配置（生产推荐）

```bash
./kv_server \
  --port=8200 \
  --raft_peers=127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202 \
  --data_path=/data/raftkv/8200 \
  --raft_sync=false \
  --raft_max_append_buffer_size=4194304 \
  --raft_enable_leader_lease=true \
  --raft_apply_batch=64 \
  --snapshot_interval_s=120
```
