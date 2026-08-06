// tests/unit/local_tso_test.cc
// LocalTso 单测：单调性、RecoverTo 不回退、批量取号不重叠、
// NextBatchBounded 上界约束，以及多线程并发唯一性。
#include "raft/local_tso.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace raftkv {
namespace {

constexpr uint64_t kFarBound = ~0ULL; // 不受上界约束时用的“无穷大”

// Next 严格单调递增
TEST(LocalTsoTest, NextIsStrictlyMonotonic) {
  LocalTso tso;
  uint64_t prev = 0;
  for (int i = 0; i < 100000; ++i) {
    uint64_t ts = tso.Next();
    ASSERT_GT(ts, prev);
    prev = ts;
  }
  EXPECT_EQ(tso.Current(), prev);
}

// Next 的高位跟随物理毫秒（同一毫秒内靠逻辑位区分）
TEST(LocalTsoTest, NextFollowsPhysicalClock) {
  LocalTso tso;
  uint64_t ts = tso.Next();
  uint64_t now_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  // 物理部分与当前墙钟毫秒差距应在很小范围内（进程调度余量 1s）
  uint64_t phys_ms = ts >> LocalTso::kLogicalBits;
  EXPECT_LE(phys_ms, now_ms + 1000);
  EXPECT_GE(phys_ms + 1000, now_ms);
}

// RecoverTo：只前进不后退（抗时钟回拨 / failover 起跳）
TEST(LocalTsoTest, RecoverToNeverGoesBackward) {
  LocalTso tso;
  uint64_t ts = tso.Next();

  tso.RecoverTo(ts - 1); // 比当前小：无效果
  EXPECT_EQ(tso.Current(), ts);

  uint64_t future = ts + (1000ULL << LocalTso::kLogicalBits); // 未来 1s
  tso.RecoverTo(future);
  EXPECT_EQ(tso.Current(), future);
  // 恢复点之后发出的号必须严格大于恢复点（模拟新 leader 从上界起跳）
  EXPECT_GT(tso.Next(), future);
}

// NextBatch：区间 [start, start+count) 与已发号不重叠、区间之间不重叠
TEST(LocalTsoTest, NextBatchIntervalsDoNotOverlap) {
  LocalTso tso;
  uint64_t single = tso.Next();

  uint64_t s1 = tso.NextBatch(100);
  EXPECT_GT(s1, single);
  uint64_t s2 = tso.NextBatch(50);
  EXPECT_GE(s2, s1 + 100); // 第二段起点在第一段之后
  EXPECT_EQ(tso.Current(), s2 + 50 - 1);
}

// NextBatchBounded：上界内正常发号，越界返回 false 且不消耗号段
TEST(LocalTsoTest, NextBatchBoundedRespectsBound) {
  LocalTso tso;
  // 先把水位推到一个确定位置，再给一个只够 10 个号的上界
  uint64_t base = tso.Next();
  uint64_t bound = tso.Current() + 10;

  uint64_t start = 0;
  ASSERT_TRUE(tso.NextBatchBounded(4, bound, &start));
  EXPECT_GT(start, base);
  EXPECT_LE(start + 4 - 1, bound);

  // 剩余额度不足：拒绝发号，且水位不动
  uint64_t before = tso.Current();
  EXPECT_FALSE(tso.NextBatchBounded(100, bound, &start));
  EXPECT_EQ(tso.Current(), before);

  // 物理时钟冲过上界的情形：bound 设为过去，任何发号都应被拒绝
  EXPECT_FALSE(tso.NextBatchBounded(1, base, &start));

  // 抬高上界后恢复发号（对应先持久化预留、再发号）
  uint64_t new_bound = tso.Current() + (1ULL << 16);
  EXPECT_TRUE(tso.NextBatchBounded(100, new_bound, &start));
  EXPECT_LE(tso.Current(), new_bound);
}

// 发出的任何号都不超过当时使用的上界（核心不变式）
TEST(LocalTsoTest, IssuedNeverExceedsBound) {
  LocalTso tso;
  uint64_t bound = tso.Next() + (1ULL << 16);
  uint64_t start = 0;
  while (tso.NextBatchBounded(7, bound, &start)) {
    ASSERT_LE(start + 7 - 1, bound);
  }
  EXPECT_LE(tso.Current(), bound);
}

// 多线程并发 Next：全局唯一
TEST(LocalTsoTest, ConcurrentNextIsUnique) {
  LocalTso tso;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;

  std::vector<std::vector<uint64_t>> results(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&tso, &results, t] {
      results[t].reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) {
        results[t].push_back(tso.Next());
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  std::set<uint64_t> all;
  for (const auto &v : results) {
    for (uint64_t ts : v) {
      ASSERT_TRUE(all.insert(ts).second) << "重复 ts: " << ts;
    }
  }
  EXPECT_EQ(all.size(), size_t(kThreads) * kPerThread);
}

// 多线程并发 NextBatchBounded：号段互不重叠，且都在上界内
TEST(LocalTsoTest, ConcurrentBoundedBatchesDoNotOverlap) {
  LocalTso tso;
  const uint64_t bound = tso.Next() + (1ULL << 20);
  constexpr int kThreads = 8;
  constexpr uint32_t kCount = 16;

  std::mutex mu;
  std::vector<std::pair<uint64_t, uint64_t>> intervals; // [start, end]
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 2000; ++i) {
        uint64_t start = 0;
        if (tso.NextBatchBounded(kCount, bound, &start)) {
          std::lock_guard<std::mutex> lk(mu);
          intervals.emplace_back(start, start + kCount - 1);
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  ASSERT_FALSE(intervals.empty());
  std::sort(intervals.begin(), intervals.end());
  for (size_t i = 0; i < intervals.size(); ++i) {
    EXPECT_LE(intervals[i].second, bound);
    if (i > 0) {
      // 前一段的末尾必须严格小于后一段的起点
      ASSERT_LT(intervals[i - 1].second, intervals[i].first);
    }
  }
}

} // namespace
} // namespace raftkv
