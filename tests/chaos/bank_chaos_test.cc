// tests/chaos/bank_chaos_test.cc
// 银行转账混沌压测驱动：N 个账户初始总额 S，多线程随机转账（事务：
// 读两账户 → 改 → 提交），并发期间由外部脚本随机 kill/重启节点。
// 审计线程周期性用最新快照 ts 读全部账户，断言总额恒等于 S。
//
// 退出码：0 = 不变量始终成立；1 = 出现总额不一致或初始化失败。
// 结束前做一次「清锁读遍历」：逐个账户 Get，把混沌期间残留的锁经
// CheckTxnStatus resolve 干净，方便随后的离线 lock/write CF 检查。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <gflags/gflags.h>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "client/kv_client.h"
#include "client/transaction.h"

DEFINE_string(peers, "127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202",
              "集群节点列表");
DEFINE_int32(accounts, 16, "账户个数");
DEFINE_int32(threads, 32, "转账线程数");
DEFINE_int32(duration_s, 30, "压测时长（秒）");
DEFINE_int64(initial, 1000, "每账户初始余额");
DEFINE_int32(audit_interval_ms, 200, "审计间隔（毫秒）");

namespace {

std::string AccountKey(int i) { return "acct_" + std::to_string(i); }

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_commit_ok{0};
std::atomic<uint64_t> g_commit_fail{0};
std::atomic<uint64_t> g_audit_ok{0};
std::atomic<uint64_t> g_audit_retry{0};
std::atomic<uint64_t> g_audit_violation{0};

raftkv::ClientOptions MakeOptions() {
  raftkv::ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.timeout_ms = 2000;
  opts.max_retry = 30;
  return opts;
}

// 用一个快照事务读全部账户并求和。
// 返回 true 且填 *sum：本轮快照读全部成功；false：这轮读失败（节点
// 正在切换等），调用方重试。账户初始化后必然存在，nullopt 视为读失败。
bool SnapshotSum(raftkv::KVClient *client, int64_t *sum) {
  raftkv::Transaction txn(client);
  if (txn.start_ts() == 0) {
    return false;
  }
  int64_t total = 0;
  for (int i = 0; i < FLAGS_accounts; ++i) {
    auto v = txn.Get(AccountKey(i));
    if (!v.has_value()) {
      return false;
    }
    total += std::stoll(*v);
  }
  *sum = total;
  return true;
}

void TransferWorker(int seed) {
  raftkv::KVClient client{MakeOptions()};
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, FLAGS_accounts - 1);
  std::uniform_int_distribution<int> amt(1, 10);

  while (!g_stop.load(std::memory_order_relaxed)) {
    int from = pick(rng), to = pick(rng);
    if (from == to) {
      continue;
    }
    int64_t amount = amt(rng);

    raftkv::Transaction txn(&client);
    if (txn.start_ts() == 0) {
      g_commit_fail.fetch_add(1);
      continue; // 集群暂不可用（选举中），稍后重试
    }
    auto fv = txn.Get(AccountKey(from));
    auto tv = txn.Get(AccountKey(to));
    if (!fv.has_value() || !tv.has_value()) {
      g_commit_fail.fetch_add(1);
      continue; // 读失败：析构 RAII 自动 Rollback
    }
    int64_t fb = std::stoll(*fv), tb = std::stoll(*tv);
    if (fb < amount) {
      continue; // 余额不足，放弃本次转账（只读事务，无需提交）
    }
    txn.Put(AccountKey(from), std::to_string(fb - amount));
    txn.Put(AccountKey(to), std::to_string(tb + amount));

    std::string err;
    if (txn.Commit(&err)) {
      g_commit_ok.fetch_add(1);
    } else {
      g_commit_fail.fetch_add(1); // 写冲突/切主，乐观模型：直接下一轮
    }
  }
}

void Auditor(int64_t expected) {
  raftkv::KVClient client{MakeOptions()};
  while (!g_stop.load(std::memory_order_relaxed)) {
    int64_t sum = 0;
    if (!SnapshotSum(&client, &sum)) {
      g_audit_retry.fetch_add(1);
    } else if (sum == expected) {
      g_audit_ok.fetch_add(1);
    } else {
      g_audit_violation.fetch_add(1);
      fprintf(stderr, "[AUDIT VIOLATION] sum=%lld expected=%lld\n",
              (long long)sum, (long long)expected);
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(FLAGS_audit_interval_ms));
  }
}

} // namespace

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const int64_t expected = (int64_t)FLAGS_accounts * FLAGS_initial;
  raftkv::KVClient client{MakeOptions()};

  // ── 初始化：一个事务写入全部账户（原子，失败重试）─────────────────
  bool inited = false;
  for (int attempt = 0; attempt < 10 && !inited; ++attempt) {
    raftkv::Transaction txn(&client);
    if (txn.start_ts() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    for (int i = 0; i < FLAGS_accounts; ++i) {
      txn.Put(AccountKey(i), std::to_string(FLAGS_initial));
    }
    std::string err;
    inited = txn.Commit(&err);
    if (!inited) {
      fprintf(stderr, "初始化重试: %s\n", err.c_str());
      // Commit 结果未知时不回滚，上一次尝试可能留下残锁；等 TTL 过期后
      // 用一次快照读触发 resolve，否则后续重试会一直撞同一把锁。
      std::this_thread::sleep_for(std::chrono::milliseconds(4000));
      raftkv::Transaction probe(&client);
      if (probe.start_ts() != 0) {
        for (int i = 0; i < FLAGS_accounts; ++i) {
          (void)probe.Get(AccountKey(i));
        }
      }
    }
  }
  if (!inited) {
    fprintf(stderr, "FAIL: 账户初始化失败\n");
    return 1;
  }
  printf("初始化完成: %d 账户 × %lld = 总额 %lld\n", FLAGS_accounts,
         (long long)FLAGS_initial, (long long)expected);

  // ── 压测 + 审计 ──────────────────────────────────────────────────
  std::vector<std::thread> workers;
  for (int i = 0; i < FLAGS_threads; ++i) {
    workers.emplace_back(TransferWorker, /*seed=*/1000 + i);
  }
  std::thread auditor(Auditor, expected);

  std::this_thread::sleep_for(std::chrono::seconds(FLAGS_duration_s));
  g_stop.store(true);
  for (auto &t : workers) {
    t.join();
  }
  auditor.join();

  // ── 清锁读遍历：把混沌残锁全部 resolve 掉，为离线 CF 检查铺路 ──────
  // （撞锁 → CheckTxnStatus → 帮提交或清残锁，都在 Transaction::Get 里）
  for (int i = 0; i < FLAGS_accounts; ++i) {
    for (int r = 0; r < 5; ++r) {
      raftkv::Transaction txn(&client);
      if (txn.start_ts() != 0 && txn.Get(AccountKey(i)).has_value()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  // ── 最终审计（必须成功且相等）────────────────────────────────────
  int64_t final_sum = -1;
  bool final_ok = false;
  for (int r = 0; r < 20 && !final_ok; ++r) {
    final_ok = SnapshotSum(&client, &final_sum);
    if (!final_ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  printf("\n========== 银行混沌压测结果 ==========\n");
  printf("提交成功: %llu   提交失败/冲突: %llu\n",
         (unsigned long long)g_commit_ok.load(),
         (unsigned long long)g_commit_fail.load());
  printf("审计通过: %llu   审计重试: %llu   审计违规: %llu\n",
         (unsigned long long)g_audit_ok.load(),
         (unsigned long long)g_audit_retry.load(),
         (unsigned long long)g_audit_violation.load());
  printf("最终总额: %lld（期望 %lld）\n", (long long)final_sum,
         (long long)expected);
  printf("======================================\n");

  if (g_audit_violation.load() != 0 || !final_ok || final_sum != expected) {
    fprintf(stderr, "FAIL: 总额不变量被破坏\n");
    return 1;
  }
  printf("PASS: 任意快照总额恒等于 %lld\n", (long long)expected);
  return 0;
}
