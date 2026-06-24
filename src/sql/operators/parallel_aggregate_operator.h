
#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "schema.pb.h"
#include "src/sql/kv_client_interface.h"
#include "src/sql/operator.h"
#include "src/sql/row_codec.h"

namespace raftsql {
struct PartialAggResult {
  int64_t count = 0;
  int64_t sum = 0;
  std::optional<int64_t> min_val;
  std::optional<int64_t> max_val;
};

class ParallelAggregateOperator : public Operator {
public:
  static constexpr int kDefaultParallelism = 4;

  ParallelAggregateOperator(KvClientInterface *client,
                            const std::string &table_name,
                            const TableSchema &schema,
                            const std::string &agg_func,
                            const std::string &agg_column,
                            int parallelism = kDefaultParallelism);

  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  // 单个 Worker 扫描指定 key 范围并计算 partial 结果
  PartialAggResult WorkerScan(const std::string &start_key,
                              const std::string &end_key);

  // 合并所有 Worker 的 partial 结果
  PartialAggResult MergeResults(const std::vector<PartialAggResult> &partials);

  KvClientInterface *client_;
  std::string table_name_;
  TableSchema schema_;
  std::string agg_func_;
  std::string agg_column_;
  int parallelism_;

  std::string result_value_;
  bool emitted_ = false;
};
} // namespace raftsql
