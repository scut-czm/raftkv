#pragma once
// TODO(9.3): 声明递归下降 Parser
// 详见 note/day9_impl.md § 完整代码实现
//
// class Parser
// ├── explicit Parser(std::string_view sql)
// ├── std::optional<Stmt> Parse()
// ├── const std::string& GetError() const
// ├── ParseSelect / ParseInsert / ParseCreate / ParseUpdate / ParseDelete
// ├── ParseOrExpr / ParseAndExpr / ParseCmpExpr / ParsePrimary
// └── Expect / Match / Peek / Advance / SetError

#include <optional>
#include <string>

#include "src/sql/ast.h"
#include "src/sql/lexer.h"

namespace raftsql {
class Parser {
public:
  explicit Parser(std::string_view sql);

  // 解析入口：根据首个关键字分派
  std::optional<Stmt> Parse();

  // 错误信息（Parse 返回 nullopt 时可查询）
  const std::string &GetError() const { return error_; }

private:
  // 各语句解析方法
  std::optional<SelectStmt> ParseSelect();
  std::optional<InsertStmt> ParseInsert();
  std::optional<CreateTableStmt> ParseCreate();
  std::optional<UpdateStmt> ParseUpdate();
  std::optional<DeleteStmt> ParseDelete();

  // 表达式解析（递归下降，处理 AND/OR 优先级）
  //   OrExpr  → AndExpr (OR AndExpr)*
  //   AndExpr → CmpExpr (AND CmpExpr)*
  //   CmpExpr → Primary (op Primary)?
  //   Primary → ident | number | string | '(' OrExpr ')'
  std::unique_ptr<Expr> ParseOrExpr();
  std::unique_ptr<Expr> ParseAndExpr();
  std::unique_ptr<Expr> ParseCmpExpr();
  std::unique_ptr<Expr> ParsePrimary();

  // 工具方法
  Token Expect(TokenType type);
  bool Match(TokenType type);
  Token Peek();
  Token Advance();
  void SetError(const std::string &msg);

  // 将标识符转大写（用于聚合函数识别）
  static std::string ToUpper(std::string s);

  Lexer lexer_;
  std::string error_;
  bool has_error_ = false;
};

} // namespace raftsql