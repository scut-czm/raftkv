// TODO: CreateTable —— 检查是否已存在，序列化 proto 写入 KV，更新缓存
// TODO: GetSchema —— 优先缓存，缓存 miss 从 KV 反序列化
// TODO: UpdateSchema —— 序列化写入 KV，更新缓存
// TODO: DropTable —— Delete KV key，清除缓存
// TODO: ListTables —— Scan __schema__/ 前缀，提取表名
// TODO: TableExists / InvalidateCache / InvalidateAllCache

#include "src/sql/schema_manager.h"

#include <cstring>
#include <string>

namespace raftsql {
SchemaManager::SchemaManager(KvClientInterface *kv_client)
    : kv_client_(kv_client) {}

std::string SchemaManager::MakeSchemaKey(const std::string &table_name) const {
  return std::string(kSchemaKeyPrefix) + table_name;
}

bool SchemaManager::CreateTable(const TableSchema &schema) {
  auto key = MakeSchemaKey(schema.table_name());
  auto existing = kv_client_->Get(key);
  if (!existing.empty()) {
    return false;
  }
  TableSchema mutable_schema = schema;
  if (mutable_schema.next_row_id() == 0) {
    mutable_schema.set_next_row_id(1);
  }
  std::string data;
  mutable_schema.SerializeToString(&data);
  kv_client_->Put(key, data);
  cache_[mutable_schema.table_name()] = mutable_schema;
  return true;
}
std::optional<TableSchema>
SchemaManager::GetSchema(const std::string &table_name) {
  auto it = cache_.find(table_name);
  if (it != cache_.end()) {
    return it->second;
  }

  auto key = MakeSchemaKey(table_name);
  auto data = kv_client_->Get(key);
  if (data.empty()) {
    return std::nullopt;
  }

  TableSchema schema;
  if (!schema.ParseFromString(data)) {
    return std::nullopt;
  }
  cache_[table_name] = schema;
  return schema;
}

bool SchemaManager::UpdateSchema(const TableSchema &schema) {
  auto key = MakeSchemaKey(schema.table_name());
  std::string data;
  schema.SerializeToString(&data);
  kv_client_->Put(key, data);
  cache_[schema.table_name()] = schema;
  return true;
}

bool SchemaManager::DropTable(const std::string &table_name) {
  auto key = MakeSchemaKey(table_name);
  kv_client_->Delete(key);
  cache_.erase(table_name);
  return true;
}

std::vector<std::string> SchemaManager::ListTables() {
  // "__schema__/" → "__schema__0"（'0'=ASCII 48 > '/'=ASCII 47)
  auto results = kv_client_->Scan(
      kSchemaKeyPrefix,
      std::string(kSchemaKeyPrefix, strlen(kSchemaKeyPrefix) - 1) + "0", 1000);
  std::vector<std::string> tables;
  for (const auto &[key, _] : results) {
    if (key.size() > strlen(kSchemaKeyPrefix)) {
      tables.push_back(key.substr(strlen(kSchemaKeyPrefix)));
    }
  }
  return tables;
}

bool SchemaManager::TableExists(const std::string &table_name) {
  return GetSchema(table_name).has_value();
}
void SchemaManager::InvalidateCache(const std::string &table_name) {
  cache_.erase(table_name);
}

void SchemaManager::InvalidateAllCache() { cache_.clear(); }
} // namespace raftsql