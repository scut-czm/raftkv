#include "client/kv_client.h"
#include "latency_histogram.h"

#include <cstdint>
#include <gflags/gflags.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <google/protobuf/stubs/strutil.h>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

DEFINE_string(peers, "127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202",
              "集群节点列表");
DEFINE_int32(threads, 16, "并发线程数");
DEFINE_int32(duration_s, 60, "持续时间（秒）");
DEFINE_int32(value_size, 1024, "value 大小（字节）");
DEFINE_double(write_ratio, 1.0, "写操作比例（0.0~1.0），1.0 表示纯写");
DEFINE_int32(scan_range, 100, "Scan 每次扫描的 key 范围");
DEFINE_string(mode, "write", "测试模式：write / readwrite / scan");
DEFINE_int32(timeout_ms, 5000, "单次 RPC 超时（毫秒）");
DEFINE_int32(max_retry, 50, "最大重试次数（含 no-leader 等待）");
DEFINE_int32(key_range, 50000, "每线程 key 空间大小，0 表示无限增长");

namespace raftkv {

// 生成固定长度的随机 value
static std::string GenerateValue(int size) {
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string result(size, ' ');
  std::mt19937 rng(std::random_device{}());
  for (int i = 0; i < size; ++i) {
    result[i] = kChars[rng() % (sizeof(kChars) - 1)];
  }
  return result;
}

// ── 纯写 / 混合读写 Worker ─────────────────────────────────────────
static void ReadWriteWorker(int thread_id, std::atomic<bool> *running,
                            std::atomic<uint64_t> *total_ops,
                            std::atomic<uint64_t> *failed_ops,
                            LatencyHistogram *write_hist,
                            LatencyHistogram *read_hist) {
  ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.linearizable = true;
  opts.timeout_ms = FLAGS_timeout_ms;
  opts.max_retry = FLAGS_max_retry;
  KVClient client(opts);

  std::string value = GenerateValue(FLAGS_value_size);
  std::mt19937 rng(thread_id);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  uint64_t local_seq = 0;

  while (running->load(std::memory_order_relaxed)) {
    uint64_t seq = FLAGS_key_range > 0 ? (local_seq++ % FLAGS_key_range) : local_seq++;
    std::string key = "perf_t" + std::to_string(thread_id) + "_" +
                      std::to_string(seq);
    auto start = std::chrono::steady_clock::now();
    bool ok = false;
    if (dist(rng) < FLAGS_write_ratio) {
      // 写操作
      ok = client.Put(key, value);
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      write_hist->Record(elapsed);
    } else {
      // 读操作（读之前写过的 key）
      std::string read_key = "perf_t" + std::to_string(thread_id) + "_" +
                             std::to_string(rng() % (local_seq + 1));
      std::string val_out;
      bool found = false;
      ok = client.Get(read_key, &val_out, &found);
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      read_hist->Record(elapsed);
    }
    if (ok) {
      total_ops->fetch_add(1, std::memory_order_relaxed);

    } else {
      failed_ops->fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// ── Scan Worker ────────────────────────────────────────────────────
static void ScanWorker(int thread_id, std::atomic<bool> *running,
                       std::atomic<uint64_t> *total_ops,
                       std::atomic<uint64_t> *failed_ops,
                       LatencyHistogram *scan_hist) {
  ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.linearizable = true;
  opts.timeout_ms = FLAGS_timeout_ms;
  opts.max_retry = FLAGS_max_retry;
  KVClient client(opts);

  uint64_t local_seq = 0;
  while (running->load(std::memory_order_relaxed)) {
    std::string start_key =
        "perf_t" + std::to_string(thread_id) + "_" + std::to_string(local_seq);
    std::string end_key = "perf_t" + std::to_string(thread_id) + "_z";

    auto start = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, std::string>> kvs;
    bool ok = client.Scan(start_key, end_key, FLAGS_scan_range, &kvs);
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    scan_hist->Record(elapsed);
    if (ok) {
      total_ops->fetch_add(1, std::memory_order_relaxed);
    } else {
      failed_ops->fetch_add(1, std::memory_order_relaxed);
    }
    local_seq += FLAGS_scan_range;
  }
}
// ── 打印结果 ───────────────────────────────────────────────────────
static void PrintStats(const std::string &label,
                       const LatencyHistogram::Stats &s) {
  if (s.count == 0)
    return;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  [" << label << "] " << "count=" << s.count
            << "  avg=" << s.avg_us / 1000.0 << "ms"
            << "  p50=" << s.p50_us / 1000.0 << "ms"
            << "  p99=" << s.p99_us / 1000.0 << "ms"
            << "  p999=" << s.p999_us / 1000.0 << "ms"
            << "  min=" << s.min_us / 1000.0 << "ms"
            << "  max=" << s.max_us / 1000.0 << "ms" << std::endl;
}

static void RunBenchmark() {
  std::cout << "========================================" << std::endl;
  std::cout << "RaftKV 性能基准测试" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "  peers:       " << FLAGS_peers << std::endl;
  std::cout << "  mode:        " << FLAGS_mode << std::endl;
  std::cout << "  threads:     " << FLAGS_threads << std::endl;
  std::cout << "  duration:    " << FLAGS_duration_s << "s" << std::endl;
  std::cout << "  value_size:  " << FLAGS_value_size << " bytes" << std::endl;
  std::cout << "  write_ratio: " << FLAGS_write_ratio << std::endl;
  std::cout << "========================================" << std::endl;

  std::atomic<bool> running{true};
  std::atomic<uint64_t> total_ops{0};
  std::atomic<uint64_t> failed_ops{0};
  LatencyHistogram write_hist, read_hist, scan_hist;

  // 启动worker线程

  std::vector<std::thread> workers;
  for (int i = 0; i < FLAGS_threads; ++i) {
    if (FLAGS_mode == "scan") {
      workers.emplace_back(ScanWorker, i, &running, &total_ops, &failed_ops,
                           &scan_hist);
    } else {
      workers.emplace_back(ReadWriteWorker, i, &running, &total_ops,
                           &failed_ops, &write_hist, &read_hist);
    }
  }
  // 每秒打印进度
  auto bench_start = std::chrono::steady_clock::now();
  for (int sec = 0; sec < FLAGS_duration_s; ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint64_t ops = total_ops.load(std::memory_order_relaxed);
    uint64_t fails = failed_ops.load(std::memory_order_relaxed);
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - bench_start)
                         .count() /
                     1000.0;
    double tps = elapsed > 0 ? ops / elapsed : 0;
    std::printf("\r  [%3ds/%ds] ops=%lu  fails=%lu  TPS=%.0f", sec + 1,
                FLAGS_duration_s, ops, fails, tps);
    std::fflush(stdout);
  }
  std::cout << std::endl;
  // 停止所有 worker
  running.store(false, std::memory_order_relaxed);
  for (auto &t : workers) {
    t.join();
  }
  // 最终统计
  auto bench_end = std::chrono::steady_clock::now();
  double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(
                         bench_end - bench_start)
                         .count() /
                     1000.0;
  uint64_t ops = total_ops.load();
  uint64_t fails = failed_ops.load();
  double tps = total_sec > 0 ? ops / total_sec : 0;
  double error_rate = (ops + fails > 0) ? 100.0 * fails / (ops + fails) : 0;
  std::cout << std::endl;
  std::cout << "======== 最终结果 ========" << std::endl;
  std::cout << "  总耗时:    " << total_sec << "s" << std::endl;
  std::cout << "  成功操作:  " << ops << std::endl;
  std::cout << "  失败操作:  " << fails << std::endl;
  std::cout << "  TPS:       " << std::fixed << std::setprecision(0) << tps
            << std::endl;

  PrintStats("Write", write_hist.Calculate());
  PrintStats("Read ", read_hist.Calculate());
  PrintStats("Scan ", scan_hist.Calculate());

  std::cout << "==========================" << std::endl;
  // 简单判定
  if (FLAGS_mode == "write" && tps >= 15000) {
    std::cout << ">>> PASS: 纯写 TPS >= 15K <<<" << std::endl;
  } else if (FLAGS_mode == "write") {
    std::cout << ">>> WARN: 纯写 TPS = " << tps << "，未达 15K 目标 <<<"
              << std::endl;
  }
  auto read_stats = read_hist.Calculate();
  if (read_stats.count > 0 && read_stats.p99_us < 8000) {
    std::cout << ">>> PASS: P99 读延迟 < 8ms <<<" << std::endl;
  } else if (read_stats.count > 0) {
    std::cout << ">>> WARN: P99 读延迟 = " << read_stats.p99_us / 1000.0
              << "ms，未达 8ms 目标 <<<" << std::endl;
  }
}

} // namespace raftkv

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  raftkv::RunBenchmark();
  return 0;
}
