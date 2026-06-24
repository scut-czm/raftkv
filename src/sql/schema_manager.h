// TODO: 定义 kSchemaKeyPrefix = "__schema__/"
// TODO: 声明 SchemaManager 类
//       public: CreateTable / GetSchema / UpdateSchema / DropTable
//               ListTables / TableExists / InvalidateCache / InvalidateAllCache
//       private: MakeSchemaKey / kv_client_ / cache_

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "schema.pb.h"
#include "src/sql/kv_client_interface.h"

namespace raftsql {
constexpr const char *kSchemaKeyPrefix = "__schema__/";

class SchemaManager {

public:
  explicit SchemaManager(KvClientInterface *kv_client);

  // 创建表；表已存在返回 false
  bool CreateTable(const TableSchema &schema);

  // 获取 Schema（优先缓存）
  std::optional<TableSchema> GetSchema(const std::string &table_name);

  // 更新 Schema（如 next_row_id 自增后回写）
  bool UpdateSchema(const TableSchema &schema);

  // 删表
  bool DropTable(const std::string &table_name);

  // 列出所有表名
  std::vector<std::string> ListTables();

  // 判断表是否存在
  bool TableExists(const std::string &table_name);

  // 清除指定表缓存
  void InvalidateCache(const std::string &table_name);
  void InvalidateAllCache();

private:
  std::string MakeSchemaKey(const std::string &table_name) const;

  KvClientInterface *kv_client_;
  std::unordered_map<std::string, TableSchema> cache_;
};
} // namespace raftsql