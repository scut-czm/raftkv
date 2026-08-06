# 第 4 步测试方案：proto + 状态机接入的完整验证

对应 checklist 的四项测试：
1. proto 重新生成、全项目编译通过
2. 旧集成测试全绿（非事务路径零回归）
3. 单节点集成：RPC 走通 Prewrite → Commit → 快照 Get
4. 三节点：follower redirect + kill leader 后事务状态不丢

---

## 1. proto 重新生成 + 全项目编译

```bash
cd ~/database_learning/raftkv
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)            # protoc 由 CMake 规则自动触发
```

检查点：
- `kv.pb.h` 中出现 `OP_TXN_PREWRITE = 5` … `OP_TXN_RESOLVE = 8`、`TxnPrewriteRequest` 等新消息；
- `GetRequest`/`ScanRequest` 有 `snapshot_ts()` 访问器；
- 旧字段编号未变（向后兼容）：用旧版客户端二进制向新版服务器发 Put/Get 应正常工作。

## 2. 旧路径零回归

```bash
cd build
ctest --output-on-failure          # 全部单测（storage/state_machine/mvcc_* 等）
# 或按套件：
./mvcc_txn_test && ./mvcc_codec_test && ./storage_test && ./state_machine_test
# 旧集成测试
bash ../tests/integration/run_integration.sh
```

通过标准：所有旧用例（snapshot_ts 恒为 0 的 Put/Get/Delete/Scan 路径）行为与改动前一致。

## 3. 单节点事务链路集成测试（新文件 `tests/integration/txn_integration_test.cc`）

沿用 GoogleTest + brpc Channel 直连的风格。测试自行拉起单节点 kv_server（conf 只含自己，起来即成为 leader）。

```cpp
// tests/integration/txn_integration_test.cc
// 单节点事务链路集成测试：真实 RPC 走 Prewrite → Commit → 快照 Get/Scan。
// 前置：测试 fixture 自行 fork 一个单节点 kv_server。

#include <brpc/channel.h>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kv.pb.h"

namespace {

constexpr int kPort = 8300;
constexpr char kServerAddr[] = "127.0.0.1:8300";

// 与 kv_service.cc 的 HybridNowTs 相同布局：高 46 位物理毫秒 << 18
uint64_t Ts(uint64_t logical) { return logical << 18; }

class TxnIntegrationTest : public ::testing::Test {
protected:
  static pid_t server_pid_;

  static void SetUpTestSuite() {
    std::string data_dir = "/tmp/raftkv_txn_it_" + std::to_string(kPort);
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    server_pid_ = fork();
    ASSERT_NE(server_pid_, -1);
    if (server_pid_ == 0) {
      // 单节点 conf：只有自己，选举后立即成为 leader
      execl("./kv_server", "kv_server",
            "--port=8300", "--ip=127.0.0.1", "--group=TxnITGroup",
            "--conf=127.0.0.1:8300:0",
            ("--data_path=" + data_dir).c_str(),
            "--raft_enable_leader_lease=true",
            "--election_timeout_ms=500",
            (char *)nullptr);
      _exit(127);
    }
    // 等 leader 选出：轮询 Put 直到 success。
    // 注意不能用弱读 Get 探活：snapshot_ts=0 且非 linearizable 的 Get
    // 不经 leader 判断，服务一起来就返回 success，探不到选主完成。
    brpc::Channel ch;
    ASSERT_EQ(ch.Init(kServerAddr, nullptr), 0);
    kv::KvService_Stub stub(&ch);
    for (int i = 0; i < 50; ++i) {
      kv::PutRequest req; req.set_key("__probe__"); req.set_value("1");
      kv::PutResponse resp; brpc::Controller cntl;
      stub.Put(&cntl, &req, &resp, nullptr);
      if (!cntl.Failed() && resp.success()) return;
      usleep(200 * 1000);
    }
    FAIL() << "server 未在 10s 内就绪";
  }

  static void TearDownTestSuite() {
    if (server_pid_ > 0) { kill(server_pid_, SIGTERM); waitpid(server_pid_, nullptr, 0); }
  }

  void SetUp() override {
    ASSERT_EQ(channel_.Init(kServerAddr, nullptr), 0);
    stub_ = std::make_unique<kv::KvService_Stub>(&channel_);
  }

  // ---- RPC 小工具 ----
  kv::TxnPrewriteResponse Prewrite(const std::string &key, const std::string &value,
                                   const std::string &primary, uint64_t start_ts,
                                   bool is_delete = false, uint64_t ttl_ms = 3000) {
    kv::TxnPrewriteRequest req;
    auto *m = req.add_mutations();
    m->set_op(is_delete ? kv::Mutation::DELETE : kv::Mutation::PUT);
    m->set_key(key); m->set_value(value);
    req.set_primary(primary); req.set_start_ts(start_ts); req.set_ttl_ms(ttl_ms);
    kv::TxnPrewriteResponse resp; brpc::Controller cntl;
    stub_->TxnPrewrite(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::TxnCommitResponse Commit(const std::string &key, uint64_t start_ts,
                               uint64_t commit_ts = 0) {
    kv::TxnCommitRequest req;
    req.add_keys(key); req.set_start_ts(start_ts); req.set_commit_ts(commit_ts);
    kv::TxnCommitResponse resp; brpc::Controller cntl;
    stub_->TxnCommit(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::TxnRollbackResponse Rollback(const std::string &key, uint64_t start_ts) {
    kv::TxnRollbackRequest req;
    req.add_keys(key); req.set_start_ts(start_ts);
    kv::TxnRollbackResponse resp; brpc::Controller cntl;
    stub_->TxnRollback(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  kv::GetResponse SnapshotGet(const std::string &key, uint64_t snapshot_ts) {
    kv::GetRequest req; req.set_key(key); req.set_snapshot_ts(snapshot_ts);
    kv::GetResponse resp; brpc::Controller cntl;
    stub_->Get(&cntl, &req, &resp, nullptr);
    EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText();
    return resp;
  }

  brpc::Channel channel_;
  std::unique_ptr<kv::KvService_Stub> stub_;
};
pid_t TxnIntegrationTest::server_pid_ = 0;

// ---- 核心链路：Prewrite → Commit → 快照可见性 ----
TEST_F(TxnIntegrationTest, PrewriteCommitSnapshotGet) {
  const uint64_t start = Ts(100), commit = Ts(200);

  auto pw = Prewrite("k1", "v1", "k1", start);
  ASSERT_TRUE(pw.success()) << pw.error();

  // 提交前，晚于 start 的快照读应报 kLocked（lock_conflict 有值）
  auto locked = SnapshotGet("k1", Ts(150));
  EXPECT_FALSE(locked.success());
  EXPECT_EQ(locked.lock_conflict().key(), "k1");
  EXPECT_EQ(locked.lock_conflict().lock_ts(), start);

  auto cm = Commit("k1", start, commit);
  ASSERT_TRUE(cm.success()) << cm.error();
  EXPECT_EQ(cm.commit_ts(), commit);

  // commit 之后的快照可见，之前的不可见
  auto after = SnapshotGet("k1", Ts(300));
  EXPECT_TRUE(after.success()); EXPECT_TRUE(after.found());
  EXPECT_EQ(after.value(), "v1");
  auto before = SnapshotGet("k1", Ts(50));
  EXPECT_TRUE(before.success()); EXPECT_FALSE(before.found());
}

// ---- Rollback：回滚后不可见，迟到 Prewrite 被墓碑挡住 ----
TEST_F(TxnIntegrationTest, RollbackBlocksLatePrewrite) {
  const uint64_t start = Ts(400);
  ASSERT_TRUE(Prewrite("k2", "v2", "k2", start).success());
  ASSERT_TRUE(Rollback("k2", start).success());

  auto g = SnapshotGet("k2", Ts(500));
  EXPECT_TRUE(g.success()); EXPECT_FALSE(g.found());

  // 同 start_ts 的迟到 Prewrite 必须被拒绝（AlreadyRolledBack）
  auto late = Prewrite("k2", "v2x", "k2", start);
  EXPECT_FALSE(late.success());
}

// ---- 写冲突：后启动事务先提交 → 先启动事务 Prewrite 失败 ----
TEST_F(TxnIntegrationTest, WriteConflict) {
  const uint64_t t1 = Ts(600), t2 = Ts(700);
  ASSERT_TRUE(Prewrite("k3", "new", "k3", t2).success());
  ASSERT_TRUE(Commit("k3", t2, Ts(800)).success());

  auto pw = Prewrite("k3", "old", "k3", t1); // t1 < 已提交的 commit_ts
  EXPECT_FALSE(pw.success());
  EXPECT_EQ(pw.conflict_commit_ts(), Ts(800));
}

// ---- 旧路径回归：snapshot_ts=0 的 Put/Get 不受事务数据影响 ----
TEST_F(TxnIntegrationTest, LegacyPathUnaffected) {
  kv::PutRequest preq; preq.set_key("legacy"); preq.set_value("plain");
  kv::PutResponse presp; brpc::Controller c1;
  stub_->Put(&c1, &preq, &presp, nullptr);
  ASSERT_FALSE(c1.Failed()); ASSERT_TRUE(presp.success());

  auto g = SnapshotGet("legacy", 0); // snapshot_ts=0 → data CF 旧路径
  EXPECT_TRUE(g.success()); EXPECT_TRUE(g.found());
  EXPECT_EQ(g.value(), "plain");

  // 事务写的 key 不会出现在旧路径读里（不同 CF）
  auto g2 = SnapshotGet("k1", 0);
  EXPECT_FALSE(g2.found());
}

} // namespace
```

CMake 追加（照抄现有集成测试目标的写法即可）：

```cmake
# 必须把 ${PROTO_SRCS} 编进目标：kv.pb.h/kv.pb.cc 由 protobuf_generate_cpp
# 的自定义命令生成，只有消费 ${PROTO_SRCS} 的目标才会触发生成并建立依赖，
# 否则并行编译时 kv.pb.h 还不存在 → fatal error: kv.pb.h: No such file。
add_executable(txn_integration_test
    tests/integration/txn_integration_test.cc
    ${PROTO_SRCS}
)
target_include_directories(txn_integration_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}    # kv.pb.h 生成在这里
    /usr/local/include
)
# 项目里没有 raftkv_lib 目标，链接方式照抄 client_test：
target_link_libraries(txn_integration_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
    GTest::GTest GTest::Main
)
# 注意：该测试依赖同目录下的 kv_server 二进制，需 make kv_server 后在 build/ 下运行
```

运行：

```bash
cd build && make kv_server txn_integration_test -j && ./txn_integration_test
```

## 4. 三节点：redirect + leader 宕机后事务状态不丢（新文件 `tests/integration/txn_failover_test.sh`）

沿用 sql_failover_test.sh 的 shell 风格，配合一个小 CLI（或直接用下面的 `txn_probe` 辅助程序；如果 client_main 已支持事务命令则用 client_main）。

```bash
#!/bin/bash
# tests/integration/txn_failover_test.sh
# 验证：1) 向 follower 发事务 RPC 返回 redirect；2) kill leader 后事务状态不丢。
set -e
DIR=$(dirname "$0")
BUILD="$DIR/../../build"

bash "$DIR/../../scripts/stop_cluster.sh" || true
rm -rf /tmp/raftkv_data_820*
bash "$DIR/../../scripts/start_cluster.sh"
sleep 3

LEADER=$(bash "$DIR/../../scripts/find_leader.sh")   # 输出形如 127.0.0.1:8201
echo "leader = $LEADER"

# ---- 测试 1：向 follower 发 TxnPrewrite，应返回 redirect=leader ----
for PORT in 8200 8201 8202; do
  ADDR="127.0.0.1:$PORT"
  [ "$ADDR" == "$LEADER" ] && continue
  OUT=$("$BUILD/txn_probe" --addr="$ADDR" --op=prewrite --key=fk --value=fv \
        --primary=fk --start_ts=$((100<<18)))
  echo "$OUT" | grep -q "redirect=$LEADER" \
    && echo "PASS: follower $ADDR redirect 正确" \
    || { echo "FAIL: follower $ADDR 未返回 redirect ($OUT)"; exit 1; }
done

# ---- 测试 2：leader 上完成 Prewrite+Commit，kill leader，新 leader 上可见 ----
"$BUILD/txn_probe" --addr="$LEADER" --op=prewrite --key=surv --value=alive \
    --primary=surv --start_ts=$((200<<18)) | grep -q success=1 || exit 1
"$BUILD/txn_probe" --addr="$LEADER" --op=commit --key=surv \
    --start_ts=$((200<<18)) --commit_ts=$((300<<18)) | grep -q success=1 || exit 1

LEADER_PORT=${LEADER##*:}
pkill -f "port=$LEADER_PORT" && echo "killed leader $LEADER"
sleep 5   # 等重新选主（election_timeout_ms=1500）

NEW_LEADER=$(bash "$DIR/../../scripts/find_leader.sh")
[ "$NEW_LEADER" != "$LEADER" ] || { echo "FAIL: 未选出新 leader"; exit 1; }
echo "new leader = $NEW_LEADER"

OUT=$("$BUILD/txn_probe" --addr="$NEW_LEADER" --op=get --key=surv \
      --snapshot_ts=$((400<<18)))
echo "$OUT" | grep -q "value=alive" \
  && echo "PASS: leader 切换后事务提交不丢" \
  || { echo "FAIL: 新 leader 上读不到已提交数据 ($OUT)"; exit 1; }

# ---- 测试 3（加强）：只 Prewrite 未 Commit 时 kill leader，锁应仍在 ----
L=$NEW_LEADER
"$BUILD/txn_probe" --addr="$L" --op=prewrite --key=pend --value=x \
    --primary=pend --start_ts=$((500<<18)) | grep -q success=1 || exit 1
pkill -f "port=${L##*:}"; sleep 5
L2=$(bash "$DIR/../../scripts/find_leader.sh")
OUT=$("$BUILD/txn_probe" --addr="$L2" --op=get --key=pend --snapshot_ts=$((600<<18)))
echo "$OUT" | grep -q "locked=1" \
  && echo "PASS: 未提交锁在 failover 后仍然存在（可被 CheckTxnStatus 清理）" \
  || { echo "FAIL: 未提交锁丢失 ($OUT)"; exit 1; }

bash "$DIR/../../scripts/stop_cluster.sh"
echo "ALL PASS"
```

`txn_probe` 辅助程序（`tests/integration/txn_probe.cc`，~100 行，也可把这些子命令并进 client_main）：

```cpp
// 用 gflags 解析 --addr/--op/--key/... ，按 op 调对应 RPC 并打印 machine-readable 结果：
//   prewrite/commit/rollback → "success=0|1 error=... redirect=..."
//   get → "success=0|1 found=0|1 value=... locked=0|1 redirect=..."
// 关键：Channel 直连 --addr，不做自动 redirect 跟随（redirect 本身就是被测行为）。
// 实现骨架与上面 fixture 中的 Prewrite/Commit/SnapshotGet 完全相同，略。
```

## 验收清单

| # | 检查项 | 命令 | 通过标准 |
|---|--------|------|----------|
| 1 | proto 兼容 + 编译 | `cmake .. && make -j` | 零错误；新枚举/消息存在 |
| 2 | 旧单测/集成 | `ctest`、`run_integration.sh` | 全绿 |
| 3 | 单节点事务链路 | `./txn_integration_test` | 4 个用例全过 |
| 4 | 三节点 redirect/failover | `bash txn_failover_test.sh` | 3 个 PASS + ALL PASS |

## 注意事项

- **stub 类名**：proto 里 service 名是 `KvService`（小写 v），生成的 stub 是 `kv::KvService_Stub`；需要 `option cc_generic_services = true;`（kv.proto 已有）。
- **就绪探活必须用写请求（或带 snapshot_ts/linearizable 的读）**：弱读 Get 在 `KVServiceImpl::Get` 里不经 `RediretIfNotLeader`，无 leader 也返回 success，不能作为“选主完成”信号；Put 会经过 leader 判断 + Raft apply，成功即说明集群可写。
- **服务器 flags**：server_main.cc 自定义了 `--port/--ip/--group/--conf/--data_path/--election_timeout_ms/--snapshot_interval_s`；`--raft_enable_leader_lease` 等是 braft 内部 gflag，直接传即可（start_cluster.sh 已这么用）。

- **时间戳布局**：测试里所有 ts 都用 `逻辑值 << 18`，与 `HybridNowTs()` 布局一致，否则 CheckTxnStatus 的 TTL 判断（`(now_ts>>18)-(lock_ts>>18)`）会算出错误的毫秒差。
- **commit_ts=0 的分配路径**也要覆盖：`Commit("k", start, /*commit_ts=*/0)` 应成功且响应里 `commit_ts() > start_ts`（leader 分配）。
- **数据目录隔离**：每次跑 failover 脚本前 `rm -rf /tmp/raftkv_data_820*`，避免上次残留的锁/墓碑影响断言。
- **redirect 断言依赖 `RediretIfNotLeader`**：follower 返回 `success=false` + `redirect=<leader>`；若 leader 未选出则是 `error="no leader"`，脚本里 sleep 3 后再取 leader 可避开该窗口。
- 测试 3 里 failover 后残锁的清理可再补一步：对新 leader 调 `CheckTxnStatus(primary=pend, lock_ts=T500)`，TTL 过期后应返回 rolled_back，然后快照读不再报锁——这验证 OP_TXN_RESOLVE 的完整闭环。
