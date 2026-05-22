#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace raftkv {

// 线程安全的延迟直方图（微秒精度）
class LatencyHistogram {
public:
  void Record(int64_t latency_us) {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.push_back(latency_us);
  }

  struct Stats {
    int64_t count = 0;
    int64_t min_us = 0;
    int64_t max_us = 0;
    int64_t avg_us = 0;
    int64_t p50_us = 0;
    int64_t p99_us = 0;
    int64_t p999_us = 0;
  };

  Stats Calculate() {
    std::lock_guard<std::mutex> lock(mu_);
    Stats s;
    if (samples_.empty()) {
      return s;
    }
    std::sort(samples_.begin(), samples_.end());
    s.count = static_cast<int64_t>(samples_.size());
    s.min_us = samples_.front();
    s.max_us = samples_.back();

    int64_t sum = 0;
    for (auto &v : samples_) {
      sum += v;
    }
    s.avg_us = sum / s.count;

    s.p50_us = Percentile(0.50);
    s.p99_us = Percentile(0.99);
    s.p999_us = Percentile(0.999);
    return s;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.clear();
  }

private:
  // 调用前必须已排序
  int64_t Percentile(double p) const {
    if (samples_.empty()) {
      return 0;
    }
    size_t idx = static_cast<size_t>(p * static_cast<double>(samples_.size()));
    if (idx >= samples_.size()) {
      idx = samples_.size() - 1;
    }
    return samples_[idx];
  }

  std::mutex mu_;
  std::vector<int64_t> samples_;
};

} // namespace raftkv