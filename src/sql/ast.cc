#include "src/sql/ast.h"
// TODO(9.2): 实现 AST 工厂方法与调试函数
// 详见 note/day9_impl.md § 完整代码实现
//
// ├── Expr::MakeColumn(name)
// ├── Expr::MakeLiteral(value)
// ├── Expr::MakeBinOp(op, left, right)
// └── StmtToString(stmt)  → std::visit 输出各 stmt 类型

namespace raftsql {
std::unique_ptr<Expr> Expr::MakeColumn(std::string name) {
  auto e = std::make_unique<Expr>();
  e->kind = Kind::kColumn;
  e->col_name = std::move(name);
  return e;
}
std::unique_ptr<Expr> Expr::MakeLiteral(std::string value) {
  auto e = std::make_unique<Expr>();
  e->kind = Kind::kLiteral;
  e->literal = std::move(value);
  return e;
}
std::unique_ptr<Expr> Expr::MakeBinOp(std::string op,
                                      std::unique_ptr<Expr> left,
                                      std::unique_ptr<Expr> right) {
  auto e = std::make_unique<Expr>();
  e->kind = Kind::kBinOp;
  e->op = std::move(op);
  e->left = std::move(left);
  e->right = std::move(right);
  return e;
}
std::string StmtToString(const Stmt &stmt) {
  return std::visit(
      [](const auto &s) -> std::string {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SelectStmt>) {
          return "SelectStmt{table=" + s.table_name + "}";
        } else if constexpr (std::is_same_v<T, InsertStmt>) {
          return "InsertStmt{table=" + s.table_name + "}";
        } else if constexpr (std::is_same_v<T, CreateTableStmt>) {
          return "CreateTableStmt{table=" + s.table_name + "}";
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
          return "UpdateStmt{table=" + s.table_name + "}";
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
          return "DeleteStmt{table=" + s.table_name + "}";
        }
        return "UnknownStmt";
      },
      stmt);
}
} // namespace raftsql