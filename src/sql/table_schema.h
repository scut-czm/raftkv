// TODO: 定义 Row 类型别名 unordered_map<string,string>
// TODO: 声明 SerializeRowJson(Row) / DeserializeRowJson(string) 函数
#pragma once

#include "schema.pb.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raftsql {
// // 列类型
// enum class ColumnType { kInt, kVarchar };

// // 列定义
// struct ColumnSchema {
//   std::string name;
//   ColumnType type;
//   int max_length = 0; // VARCHAR(N) 的 N
//   bool is_primary_key = false;
// };

// // 表 Schema
// struct TableSchema {
//   std::string table_name;
//   std::vector<ColumnSchema> columns;

//   // 查找列索引
//   std::optional<size_t> FindColum(const std::string &col_name) const;

//   // 获取主键列名
//   std::string GetPrimaryKeyColumn() const;

//   // 序列化/反序列化（JSON 格式，存储在 RaftKV 中）
//   // Key = "__schema__/{table_name}"
//   std::string Serialize() const;

//   static std::optional<TableSchema> Deserialize(const std::string &data);
// };
// 行数据
// 行数据：列名 → 字符串值（SQL 层内部统一格式）
using Row = std::unordered_map<std::string, std::string>;

// 行编码方案：
// Key = "{table_name}/{primary_key_value}"
// Value = JSON 序列化的行 {"col1": "v1", "col2": "v2"}
// 简单 JSON 编码（不依赖第三方库，手写轻量 JSON）

// 行序列化/反序列化
std::string SerializeRow(const Row &row, const TableSchema &schema);
Row Deserialize(const std::string &data, const TableSchema &schema);

} // namespace raftsql