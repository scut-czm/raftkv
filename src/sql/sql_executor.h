// TODO: 定义 QueryResult 结构体（ok / error_msg / rows / affected_rows）
// TODO: 声明 SQLExecutor 类
//       public:  explicit SQLExecutor(KvClientInterface*) / Execute(sql)
//       private: ExecuteCreate / ExecuteInsert / ExecuteSelect / ExecuteUpdate
//       / ExecuteDelete
//                EvalPredicate / GetExprValue / CompareValues
//                kv_client_ / schema_manager_ / row_id_allocator_

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/sql/ast.h"
#include "src/sql/kv_client_interface.h"
#include "src/sql/row_codec.h"
#include "src/sql/row_id_allocator.h"
#include "src/sql/schema_manager.h"

namespace raftsql {
class SQLExecutor {
public:
  struct QueryResult {
    bool ok = false;
    std::string error_msg;
    std::vector<Row> rows;
    int64_t affected_rows = 0;
  };

  explicit SQLExecutor(KvClientInterface *kv_client);

  // 主入口：执行 SQL 字符串
  QueryResult Execute(const std::string &sql);

private:
  QueryResult ExecuteCreate(const CreateTableStmt &stmt);
  QueryResult ExecuteInsert(const InsertStmt &stmt);
  QueryResult ExecuteSelect(const SelectStmt &stmt);
  QueryResult ExecuteUpdate(const UpdateStmt &stmt);
  QueryResult ExecuteDelete(const DeleteStmt &stmt);

  // 谓词求值（供 UPDATE/DELETE 使用）
  bool EvalPredicate(const Expr *expr, const Row &row) const;
  std::string GetExprValue(const Expr *expr, const Row &row) const;
  bool CompareValues(const std::string &left, const std::string &op,
                     const std::string &right) const;

  KvClientInterface *kv_client_;
  SchemaManager schema_manager_;
  RowIdAllocator row_id_allocator_;
};
} // namespace raftsql