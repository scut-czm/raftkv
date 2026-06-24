#pragma once
// TODO(9.2): 定义 AST 节点
// 详见 note/day9_impl.md § 完整代码实现
//
// ├── struct ColumnSpec    { name, type, primary_key }
// ├── struct Expr          { Kind, col_name/literal/op/left/right }
// │   └── 工厂方法: MakeColumn / MakeLiteral / MakeBinOp
// ├── struct SelectStmt    { columns, table_name, where_expr, agg_func,
// agg_column } ├── struct InsertStmt    { table_name, columns, values } ├──
// struct CreateTableStmt { table_name, columns } ├── struct UpdateStmt    {
// table_name, set_clauses, where_expr } ├── struct DeleteStmt    { table_name,
// where_expr } └── using Stmt = std::variant<SelectStmt, InsertStmt,
// CreateTableStmt,
//                               UpdateStmt, DeleteStmt>

#include <memory>
#include <rocksdb/advanced_options.h>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace raftsql {
// 列定义（CREATE TABLE 用）
struct ColumnSpec {
  std::string name;
  std::string type; // "INT" / "VARCHAR(255)"
  bool primary_key = false;
};
// 表达式节点（WHERE 条件）
struct Expr {
  enum class Kind { kColumn, kLiteral, kBinOp };
  Kind kind;

  // kColumn
  std::string col_name;

  // kLiteral（数字或字符串字面量）
  std::string literal;

  // kBinOp: =, >, <, >=, <=, !=, AND, OR
  std::string op;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;

  // 工厂方法
  static std::unique_ptr<Expr> MakeColumn(std::string name);
  static std::unique_ptr<Expr> MakeLiteral(std::string value);
  static std::unique_ptr<Expr> MakeBinOp(std::string op,
                                         std::unique_ptr<Expr> left,
                                         std::unique_ptr<Expr> right);
};
// SELECT col1, col2 FROM t WHERE ...
struct SelectStmt {
  std::vector<std::string> columns; // 空 = SELECT *
  std::string table_name;
  std::unique_ptr<Expr> where_expr; // nullptr = 无 WHERE
  // 聚合函数支持（Day 16）
  std::string agg_func;   // "COUNT" / "SUM" / "MIN" / "MAX"（空=非聚合
  std::string agg_column; // 聚合列名（"*" 用于 COUNT(*)）
};

// INSERT INTO t (col1, col2) VALUES (v1, v2)
struct InsertStmt {
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<std::string> values;
};

// CREATE TABLE t (col1 INT, col2 VARCHAR(255), PRIMARY KEY(col1))
struct CreateTableStmt {
  std::string table_name;
  std::vector<ColumnSpec> columns;
};

// UPDATE t SET col1 = v1, col2 = v2 WHERE ...
struct UpdateStmt {
  std::string table_name;
  std::vector<std::pair<std::string, std::string>> set_clauses;
  std::unique_ptr<Expr> where_expr;
};

// DELETE FROM t WHERE ...
struct DeleteStmt {
  std::string table_name;
  std::unique_ptr<Expr> where_expr;
};

using Stmt = std::variant<SelectStmt, InsertStmt, CreateTableStmt, UpdateStmt,
                          DeleteStmt>;

// 调试用：打印 AST
std::string StmtToString(const Stmt &stmt);

} // namespace raftsql