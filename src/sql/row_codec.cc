// TODO: EncodeRowKey —— "{table}:{row_id:08d}"
// TODO: DecodeRowKey —— rfind(':') 分割
// TODO: TableScanRange —— {table+":", table+";"}
// TODO: EncodeRow —— Row → RowData proto → SerializeToString
// TODO: DecodeRow —— ParseFromString → RowData → Row（跳过 is_null）
// TODO: StringToValue —— 按 DataType 调用 set_int_val/set_str_val 等
// TODO: ValueToString —— val_case() switch
// TODO: GetPrimaryKeyColumn —— 找第一个 primary_key=true 的列名

#include "src/sql/row_codec.h"

#include <charconv> // 【核心注入】引入 C++17 无锁、无分配的高性能字节流数值转换原语
#include <stdexcept>
#include <string>

namespace raftsql {
// =========================================================================
// 优化点 1：原生快速填充，彻底干掉 snprintf
// 运行时格式化控制符解析与内部缓存开销
// =========================================================================

std::string RowCodec::EncodeRowKey(const std::string &table_name,
                                   int64_t row_id) {
  std::string result;
  // 核心内幕：预先锁死分配空间（表名长 + 1个冒号 +
  // 至少8位自增ID），热路径绝不发生二次内存重构
  result.reserve(table_name.size() + 1 + 8);
  result.append(table_name).append(":");

  char buf[32];
  char *p = buf + 32;
  int64_t v = row_id;
  int count = 0;

  // 逆向剥离数字字节入栈，纯粹的低级位运算
  do {
    *(--p) = '0' + (v % 10);
    v /= 10;
    count++;
  } while (v > 0);
  // 高位精准补零对齐，强行规避 ASCII 字典序越界风险
  while (count < 8) {
    *(--p) = '0';
    count++;
  }
  // 批量直接刷入 string 尾部
  result.append(p, buf + 32 - p);
  return result;
}

// =========================================================================
// 优化点 2：采用 std::string_view 视窗传递与 std::from_chars 就地解析，达成 0
// Malloc 终极形态
// =========================================================================
bool RowCodec::DecodeRowKey(std::string_view key, std::string_view *table_name,
                            int64_t *row_id) {

  // 1. 倒序寻找隔离冒号
  auto pos = key.rfind(':');
  if (pos == std::string_view::npos) {
    return false;
  }
  // 2. 物理割裂表名：挪动指针即可，不触发任何 OS 级别的临时空间申请
  if (table_name) {
    *table_name = key.substr(0, pos);
  }
  // 3. 数值还原：彻底清洗掉旧的 key.substr 离散生成带来的 string 拷贝开销
  if (row_id) {
    std::string_view id_view = key.substr(pos + 1);
    // 配合 <charconv>
    // 双指针原语：直接在原始网络/存储缓冲区字节流上做数值撞击，速度提高数倍且绝不抛出异常
    auto [ptr, ec] = std::from_chars(id_view.data(),
                                     id_view.data() + id_view.size(), *row_id);
    if (ec != std::errc()) {
      return false;
    }
  }
  return true;
}

std::pair<std::string, std::string>
RowCodec::TableScanRange(const std::string &table_name) {
  return {table_name + ":",
          table_name +
              ";"}; // ';' (ASCII 59) > ':' (ASCII 58)，保证覆盖所有行 key
}

std::string RowCodec::EncodeRow(const Row &row, const TableSchema &schema) {
  RowData proto;
  for (const auto &col_def : schema.columns()) {
    auto it = row.find(col_def.name());
    if (it == row.end()) {
      Value null_val;
      null_val.set_is_null(true);
      (*proto.mutable_columns())[col_def.name()] = null_val;
      continue;
    }
    (*proto.mutable_columns())[col_def.name()] =
        StringToValue(it->second, col_def.type());
  }
  std::string data;
  proto.SerializeToString(&data);
  return data;
}

Row RowCodec::DecodeRow(const std::string &data, const TableSchema &schema) {
  RowData proto;
  if (!proto.ParseFromString(data)) {
    return {};
  }
  Row row;
  for (const auto &[col_name, val] : proto.columns()) {
    if (val.is_null()) {
      continue;
    }
    row[col_name] = ValueToString(val);
  }
  return row;
}
std::string RowCodec::GetPrimaryKeyColumn(const TableSchema &schema) {
  for (const auto &col : schema.columns()) {
    if (col.primary_key()) {
      return col.name();
    }
  }
  return "";
}

Value RowCodec::StringToValue(const std::string &str_val, DataType type) {
  Value val;
  switch (type) {
  case DT_INT:
    try {
      val.set_int_val(std::stoll(str_val));
    } catch (...) {
      val.set_is_null(true);
    }
    break;
  case DT_VARCHAR:
    val.set_str_val(str_val);
    break;

  case DT_FLOAT:
    try {
      val.set_float_val(std::stod(str_val));
    } catch (...) {
      val.set_is_null(true);
    }
    break;

  case DT_BOOL:
    val.set_bool_val(str_val == "true" || str_val == "1");
    break;
  default:
    val.set_str_val(str_val);
    break;
  }
  return val;
}

std::string RowCodec::ValueToString(const Value &val) {
  if (val.is_null()) {
    return "NULL";
  }
  switch (val.val_case()) {
  case Value::kIntVal:
    return std::to_string(val.int_val());
  case Value::kStrVal:
    return val.str_val();
  case Value::kFloatVal:
    return std::to_string(val.float_val());
  case Value::kBoolVal:
    return val.bool_val() ? "true" : "false";
  default:
    return "";
  }
}
} // namespace raftsql