// TODO: 声明 RowCodec 类（全静态方法）
//       Key: EncodeRowKey / DecodeRowKey / TableScanRange
//       Value: EncodeRow / DecodeRow
//       类型转换: StringToValue / ValueToString / GetPrimaryKeyColumn

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view> // 【注入】引入现代视窗管理
#include <unordered_map>
#include <utility>

#include "row.pb.h"
#include "schema.pb.h"
#include "src/sql/table_schema.h"

namespace raftsql {
class RowCodec {
public:
  // ===== Key 编解码 =====

  // 生成行 Key: "{table_name}:{row_id:08d}"
  static std::string EncodeRowKey(const std::string &table_name,
                                  int64_t row_id);

  // 解析行 Key，提取 table_name 和 row_id
  // 【物理重构】入参改为 string_view，提取出的 table_name 升级为 string_view*
  // 指针 从而使得从底层存储捞上来的 Key 在切片提取表名时，物理开销彻底降为 0
  // 拷贝
  static bool DecodeRowKey(std::string_view key, std::string_view *table_name,
                           int64_t *row_id);

  // 生成表的 Scan 范围 ["{table}:", "{table};")
  static std::pair<std::string, std::string>
  TableScanRange(const std::string &table_name);

  // ===== Value 编解码 =====
  static std::string EncodeRow(const Row &row, const TableSchema &schema);

  // Protobuf 字符串 → Row
  static Row DecodeRow(const std::string &data, const TableSchema &schema);

  // ===== 类型转换 =====
  static Value StringToValue(const std::string &str_val, DataType type);
  static std::string ValueToString(const Value &val);

  // 获取 Schema 的主键列名（空=无主键）
  static std::string GetPrimaryKeyColumn(const TableSchema &schema);
};

} // namespace raftsql