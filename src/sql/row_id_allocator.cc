// TODO: Init —— 设置 current_ 和 limit_ 为 persisted_limit
// TODO: Allocate —— fetch_add(1) 快路径；超出 limit_ 时调 RefillSegment
// TODO: RefillSegment —— lock_guard，双重检查，new_limit =
// limit_+kSegmentSize，调 raft_persist_fn_

#include "src/sql/row_id_allocator.h"

namespace raftsql {

RowIdAllocator::RowIdAllocator(std::function<bool(int64_t)> raft_persist_fn)
    : raft_persist_fn_(std::move(raft_persist_fn)) {}

void RowIdAllocator::Init(int64_t persisted_limit) {
  current_.store(persisted_limit, std::memory_order_relaxed);
  limit_ = persisted_limit;
}

int64_t RowIdAllocator::Allocate() {
  int64_t id = current_.fetch_add(1, std::memory_order_relaxed);
  if (id < limit_) {
    return id; // 快路径无锁
  }
  RefillSegment();
  return id;
}
void RowIdAllocator::RefillSegment() {
  std::lock_guard<std::mutex> lock(refill_mu_);
  if (current_.load(std::memory_order_relaxed) < limit_) {
    return;
  }
  int64_t new_limit = limit_ + kSegmentSize;
  raft_persist_fn_(new_limit);
  limit_ = new_limit;
}
} // namespace raftsql