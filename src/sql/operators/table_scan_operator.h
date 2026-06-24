// TODO: 声明 TableScanOperator : Operator
//       构造：kv_client / table_name / schema / scan_predicate
//       private: CanNarrowScanRange / NarrowScanRange / EvalPredicate
//                GetExprValue / CompareValues
//                scan_results_ / cursor_

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "schema.pb.h"
#include "src/sql/ast.h"
#include "src/sql/kv_client_interface.h"
#include "src/sql/operator.h"
#include "src/sql/row_codec.h"

namespace raftsql {

class TableScanOperator : public Operator {
public:
  TableScanOperator(KvClientInterface *kv_client, std::string table_name,
                    TableSchema schema, const Expr *scan_predicate = nullptr);

  void Open() override;
  bool Next(Row *row) override;
  void Close() override;

private:
  // 判断谓词是否可以缩小 Scan 范围（仅主键等值/范围条件）
  bool CanNarrowScanRange(const Expr *expr) const;

  // 根据主键谓词生成缩小后的 Scan 范围
  std::pair<std::string, std::string> NarrowScanRange(const Expr *expr) const;

  // 谓词求值（类型感知）
  bool EvalPredicate(const Expr *expr, const Row &row) const;
  std::string GetExprValue(const Expr *expr, const Row &row) const;
  bool CompareValues(const std::string &left, const std::string &op,
                     const std::string &right) const;

  KvClientInterface *kv_client_;
  std::string table_name_;
  TableSchema schema_;
  const Expr *scan_predicate_;

  std::vector<std::pair<std::string, std::string>> scan_results_;
  size_t cursor_ = 0;
};





} // namespace raftsql