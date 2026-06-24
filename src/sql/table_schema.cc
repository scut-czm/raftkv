// TODO: SerializeRowJson —— {"col":"val",...} 格式输出
// TODO: DeserializeRowJson —— 简单 JSON 解析，提取 key/value 对

#include "src/sql/table_schema.h"

#include <algorithm>
#include <charconv> // C++17 高性能字符转换
// #include <format>   // C++20 现代格式化
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view> // C++17

namespace raftsql {

#if 0
// ==========================================
// 局部辅助解析工具：利用 string_view 实现零拷贝提取
// ==========================================

namespace {

// 提取带双引号的字符串字段值 (如 "name":"users" -> 返回 users)
std::string_view ExtractStringField(std::string_view json,
                                    std::string_view key) {
  size_t key_pos = json.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  size_t colon = json.find(':', key_pos);
  size_t start = json.find('"', colon);
  size_t end = json.find('"', start + 1);
  if (start == std::string_view::npos || end == std::string_view::npos)
    return {};
  return json.substr(start + 1, end - start - 1); // 零拷贝切片
}
// 提取不带双引号的标量值 (如 "max_length":255 -> 返回 255)
std::string_view ExtractValueField(std::string_view json,
                                   std::string_view key) {
  size_t key_pos = json.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  size_t colon = json.find(':', key_pos);
  size_t end = json.find_first_of(",}", colon);
  if (colon == std::string_view::npos || end == std::string_view::npos)
    return {};
  return json.substr(colon + 1, end - colon - 1); // 零拷贝切片
}
} // namespace

// ==========================================
// TableSchema 核心成员函数现代化改写
// ==========================================

std::optional<size_t>
TableSchema::FindColum(const std::string &col_name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == col_name) {
      return i;
    }
  }
  return std::nullopt;
}

std::string TableSchema::GetPrimaryKeyColumn() const {
  for (const auto &col : columns) {
    if (col.is_primary_key) {
      return col.name;
    }
  }
  return ""; // 如果没有明确的主键，返回空字符串
}

// std::string TableSchema::Serialize() const {
//   std::string res = "{\"table_name\":\"" + table_name + "\",\"columns\":[";
//   for (size_t i = 0; i < columns.size(); ++i) {
//     if (i > 0) {
//       res += ",";
//     }
//     res += "{\"name\":\"" + columns[i].name + "\",";
//     res +=
//         "\"type\":\"" +
//         std::string(columns[i].type == ColumnType::kInt ? "INT" : "VARCHAR")
//         +
//         "\",";
//     res += "\"max_length\":" + std::to_string(columns[i].max_length) + ",";
//     res += "\"primary_key\":" +
//            std::string(columns[i].is_primary_key ? "true" : "false") + "}";
//   }
//   res += "]}";
//   return res;
// }

// 使用 C++20 std::format 改写序列化：消灭 temporary string 与 std::to_string
std::string TableSchema::Serialize() const {
  std::string cols_str;
  cols_str.reserve(columns.size() *
                   128); // 提前预留内存，防止后续扩容引发重新分配

  for (size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) {
      cols_str += ",";
    }
    // C++20 std::format 允许在原始字面量中通过 {{ 和 }} 转义大括号
    cols_str += std::format(
        R"({{"name":"{}","type":"{}","max_length":{},"primary_key":{}}})",
        columns[i].name,
        columns[i].type == ColumnType::kInt ? "INT" : "VARCHAR",
        columns[i].max_length, columns[i].is_primary_key ? "true" : "false");
  }
  return std::format(R"({{"table_name":"{}","columns":[{}]}})", table_name,
                     cols_str);
}

// std::optional<TableSchema> TableSchema::Deserialize(const std::string &data)
// {
//   if (data.empty()) {
//     return std::nullopt;
//   }
//   TableSchema schema;

//   // 1. 解析表名 table_name
//   size_t tn_pos = data.find("\"table_name\"");
//   if (tn_pos == std::string::npos) {
//     return std::nullopt;
//   }
//   size_t tn_colon = data.find(":", tn_pos);
//   size_t tn_start = data.find("\"", tn_colon);
//   size_t tn_end = data.find("\"", tn_start + 1);
//   if (tn_start == std::string::npos || tn_end == std::string::npos) {
//     return std::nullopt;
//   }
//   schema.table_name = data.substr(tn_start + 1, tn_end - tn_start - 1);

//   // 2. 定位 columns 数组边界
//   size_t col_pos = data.find("\"columns\"");
//   if (col_pos == std::string::npos) {
//     return std::nullopt;
//   }
//   size_t array_start = data.find("[", col_pos);
// }

// 使用 C++17 string_view 全程零拷贝反序列化
std::optional<TableSchema> TableSchema::Deserialize(const std::string &data) {
  if (data.empty()) {
    return std::nullopt;
  }
  // 转换成无状态的 string_view 视窗
  std::string_view data_view(data);
  TableSchema schema;

  // 1. 提取表名
  auto tn_view = ExtractStringField(data_view, "\"table_name\"");
  if (tn_view.empty()) {
    return std::nullopt;
  }
  schema.table_name = std::string(tn_view); // 仅在此处发生单次内存分配

  // 2. 定位 columns 数组
  size_t col_pos = data_view.find("\"columns\"");
  if (col_pos == std::string_view::npos) {
    return std::nullopt;
  }
  size_t array_start = data_view.find("[", col_pos);
  size_t array_end = data_view.find("]", array_start);
  if (array_start == std::string_view::npos ||
      array_end == std::string_view::npos) {
    return std::nullopt;
  }

  // 3. 遍历提取
  size_t obj_start = array_start;
  while ((obj_start = data_view.find("{", obj_start)) != std::string::npos &&
         obj_start < array_end) {
    size_t obj_end = data_view.find("}", obj_start);
    if (obj_end == std::string_view::npos || obj_end > array_end) {
      break;
    }
    // 获取当前列对象的匿名视窗
    std::string_view obj_view =
        data_view.substr(obj_start, obj_end - obj_start + 1);
    ColumnSchema col;

    // 提取并填充列名
    auto n_view = ExtractStringField(obj_view, "\"name\"");
    col.name = std::string(n_view);

    // 提取类型
    auto t_view = ExtractStringField(obj_view, "\"type\"");
    col.type = (t_view == "INT") ? ColumnType::kInt : ColumnType::kVarchar;

    // 提取最大长度：利用 C++17 std::from_chars 代替 std::stoi
    auto ml_view = ExtractValueField(obj_view, "\"max_length\"");
    if (!ml_view.empty()) {
      // std::from_chars 直接无视没有空字符结尾的 view 字节切片，极致性能
      std::from_chars(ml_view.data(), ml_view.data() + ml_view.size(),
                      col.max_length);
    }

    // 提取主键标识
    auto pk_view = ExtractValueField(obj_view, "\"primary_key\"");
    col.is_primary_key = (pk_view == "true");

    schema.columns.push_back(std::move(col));
    obj_start = obj_end + 1;
  }
  return schema;
}

#endif

// ==========================================
// 行数据（Row）的现代化序列化
// ==========================================
#if 0
std::string SerializeRow(const Row &row, const TableSchema &schema) {
  std::string res = "{";
  bool first = true;
  for (const auto &col : schema.columns) {
    auto it = row.find(col.name);
    if (it != row.end()) {
      if (!first) {
        res += ",";
      }
      res += std::format(R"("{}":"{}")", col.name, it->second);
      first = false;
    }
  }
  res += "}";
  return res;
}

Row DeserializeRow(const std::string &data, const TableSchema & /*schema*/) {
  Row row;
  if (data.empty() || data == "{}") {
    return row;
  }
  std::string_view data_view(data);
  size_t pos = 0;

  while (pos < data_view.size()) {
    size_t k_start = data_view.find('"', pos);
    if (k_start == std::string_view::npos) {
      break;
    }
    size_t k_end = data_view.find('"', k_start + 1);
    if (k_end == std::string_view::npos) {
      break;
    }
    std::string_view key = data_view.substr(k_start + 1, k_end - k_start - 1);

    size_t colon = data_view.find(':', k_end + 1);
    if (colon == std::string_view::npos) {
      break;
    }
    size_t v_start = data_view.find('"', colon + 1);
    if (v_start == std::string_view::npos)
      break;
    size_t v_end = data_view.find('"', v_start + 1);
    if (v_end == std::string_view::npos)
      break;
    std::string_view val = data_view.substr(v_start + 1, v_end - v_start - 1);

    row[std::string(key)] = std::string(val);
    pos = v_end + 1;
  }
  return row;
}
#endif
std::string SerializeRowJson(const Row &row) {
  std::ostringstream oss;
  oss << "{";
  bool first = true;
  for (const auto &[col, val] : row) {
    if (!first)
      oss << ",";
    first = false;
    oss << "\"" << col << "\":\"" << val << "\"";
  }
  oss << "}";
  return oss.str();
}

Row DeserializeRowJson(const std::string &data) {
  Row row;
  // 简单 JSON 解析：{"key":"value",...}
  size_t pos = 0;
  auto skip = [&]() {
    while (pos < data.size() && (data[pos] == ' ' || data[pos] == '{' ||
                                 data[pos] == '}' || data[pos] == ','))
      ++pos;
  };
  auto readStr = [&]() -> std::string {
    if (pos >= data.size() || data[pos] != '"')
      return "";
    ++pos;
    std::string s;
    while (pos < data.size() && data[pos] != '"') {
      s += data[pos++];
    }
    if (pos < data.size())
      ++pos; // skip closing '"'
    return s;
  };

  skip();
  while (pos < data.size() && data[pos] != '}') {
    std::string key = readStr();
    if (pos < data.size() && data[pos] == ':')
      ++pos;
    std::string val = readStr();
    if (!key.empty())
      row[key] = val;
    skip();
  }
  return row;
}
} // namespace raftsql
