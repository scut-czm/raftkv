## 完整代码实现

### `src/sql/lexer.h`

```cpp
#pragma once

#include <string>
#include <string_view>

namespace raftsql {
enum class TokenType {
  // 关键字
  kSelect,
  kFrom,
  kWhere,
  kInsert,
  kInto,
  kValues,
  kCreate,
  kTable,
  kUpdate,
  kSet,
  kDelete,
  kInt,
  kVarchar,
  kPrimary,
  kKey,
  // 标识符与字面量
  kIdent,
  kNumber,
  kString,
  // 符号
  kStar,
  kComma,
  kLParen,
  kRParen,
  kSemicolon,
  kDot,
  // 比较运算符
  kEq,
  kNeq,
  kLt,
  kGt,
  kLe,
  kGe,
  // 逻辑运算符
  kAnd,
  kOr,
  // 结束
  kEof
};
// Token 类型名称（调试用）
const char *TokenTypeName(TokenType type);

struct Token {
  TokenType type;
  std::string value; // 原始文本（标识符保留原大小写）
};
class Lexer {
public:
  explicit Lexer(std::string_view input);

  // 消费并返回下一个 Token
  Token NextToken();

  // 预看下一个 Token（不消费）
  Token PeekToken();

private:
  void SkipWhitespace();
  Token ScanIdentOrKeyword();
  Token ScanNumber();
  Token ScanString(); // 处理 'xxx' 单引号字符串
  Token ScanOperator();

  // 关键字匹配：将 ident 转大写后查表
  static TokenType LookupKeyword(const std::string &ident);

  std::string_view input_;
  size_t pos_ = 0;
  bool has_peeked_ = false;
  Token peeked_token_;
};
} // namespace raftsql
```

### `src/sql/lexer.cc`

```cpp
#include "src/sql/lexer.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace raftsql {

static const std::unordered_map<std::string, TokenType> kKeywords = {
    {"SELECT", TokenType::kSelect},   {"FROM", TokenType::kFrom},
    {"WHERE", TokenType::kWhere},     {"INSERT", TokenType::kInsert},
    {"INTO", TokenType::kInto},       {"VALUES", TokenType::kValues},
    {"CREATE", TokenType::kCreate},   {"TABLE", TokenType::kTable},
    {"UPDATE", TokenType::kUpdate},   {"SET", TokenType::kSet},
    {"DELETE", TokenType::kDelete},   {"INT", TokenType::kInt},
    {"VARCHAR", TokenType::kVarchar}, {"PRIMARY", TokenType::kPrimary},
    {"KEY", TokenType::kKey},         {"AND", TokenType::kAnd},
    {"OR", TokenType::kOr},
};

const char* TokenTypeName(TokenType type) {
  switch (type) {
    case TokenType::kSelect:    return "SELECT";
    case TokenType::kFrom:      return "FROM";
    case TokenType::kWhere:     return "WHERE";
    case TokenType::kInsert:    return "INSERT";
    case TokenType::kInto:      return "INTO";
    case TokenType::kValues:    return "VALUES";
    case TokenType::kCreate:    return "CREATE";
    case TokenType::kTable:     return "TABLE";
    case TokenType::kUpdate:    return "UPDATE";
    case TokenType::kSet:       return "SET";
    case TokenType::kDelete:    return "DELETE";
    case TokenType::kInt:       return "INT";
    case TokenType::kVarchar:   return "VARCHAR";
    case TokenType::kPrimary:   return "PRIMARY";
    case TokenType::kKey:       return "KEY";
    case TokenType::kIdent:     return "IDENT";
    case TokenType::kNumber:    return "NUMBER";
    case TokenType::kString:    return "STRING";
    case TokenType::kStar:      return "STAR";
    case TokenType::kComma:     return "COMMA";
    case TokenType::kLParen:    return "LPAREN";
    case TokenType::kRParen:    return "RPAREN";
    case TokenType::kSemicolon: return "SEMICOLON";
    case TokenType::kDot:       return "DOT";
    case TokenType::kEq:        return "EQ";
    case TokenType::kNeq:       return "NEQ";
    case TokenType::kLt:        return "LT";
    case TokenType::kGt:        return "GT";
    case TokenType::kLe:        return "LE";
    case TokenType::kGe:        return "GE";
    case TokenType::kAnd:       return "AND";
    case TokenType::kOr:        return "OR";
    case TokenType::kEof:       return "EOF";
    default:                    return "UNKNOWN";
  }
}

Lexer::Lexer(std::string_view input) : input_(input), pos_(0) {}

void Lexer::SkipWhitespace() {
  while (pos_ < input_.size() &&
         std::isspace(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }
}

Token Lexer::ScanIdentOrKeyword() {
  size_t start = pos_;
  while (pos_ < input_.size() &&
         (std::isalnum(static_cast<unsigned char>(input_[pos_])) ||
          input_[pos_] == '_')) {
    ++pos_;
  }
  std::string ident(input_.substr(start, pos_ - start));
  TokenType type = LookupKeyword(ident);
  return {type, ident};
}

Token Lexer::ScanNumber() {
  size_t start = pos_;
  while (pos_ < input_.size() &&
         std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
    ++pos_;
  }
  return {TokenType::kNumber,
          std::string(input_.substr(start, pos_ - start))};
}

Token Lexer::ScanString() {
  ++pos_;  // skip opening '
  size_t start = pos_;
  std::string value;
  while (pos_ < input_.size() && input_[pos_] != '\'') {
    if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
      ++pos_;  // skip backslash
      value += input_[pos_];
    } else {
      value += input_[pos_];
    }
    ++pos_;
  }
  if (pos_ < input_.size()) ++pos_;  // skip closing '
  return {TokenType::kString, value};
}

Token Lexer::ScanOperator() {
  char c = input_[pos_++];
  switch (c) {
    case '<':
      if (pos_ < input_.size()) {
        if (input_[pos_] == '=') { ++pos_; return {TokenType::kLe, "<="}; }
        if (input_[pos_] == '>') { ++pos_; return {TokenType::kNeq, "<>"}; }
      }
      return {TokenType::kLt, "<"};
    case '>':
      if (pos_ < input_.size() && input_[pos_] == '=') {
        ++pos_;
        return {TokenType::kGe, ">="};
      }
      return {TokenType::kGt, ">"};
    case '=':
      return {TokenType::kEq, "="};
    case '!':
      if (pos_ < input_.size() && input_[pos_] == '=') {
        ++pos_;
        return {TokenType::kNeq, "!="};
      }
      return {TokenType::kEof, ""};
    case '*': return {TokenType::kStar, "*"};
    case ',': return {TokenType::kComma, ","};
    case '(': return {TokenType::kLParen, "("};
    case ')': return {TokenType::kRParen, ")"};
    case ';': return {TokenType::kSemicolon, ";"};
    case '.': return {TokenType::kDot, "."};
    default:  return {TokenType::kEof, ""};
  }
}

TokenType Lexer::LookupKeyword(const std::string& ident) {
  std::string upper = ident;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
  auto it = kKeywords.find(upper);
  if (it != kKeywords.end()) return it->second;
  return TokenType::kIdent;
}

Token Lexer::NextToken() {
  if (has_peeked_) {
    has_peeked_ = false;
    return peeked_token_;
  }
  SkipWhitespace();
  if (pos_ >= input_.size()) return {TokenType::kEof, ""};

  char c = input_[pos_];
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    return ScanIdentOrKeyword();
  }
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return ScanNumber();
  }
  if (c == '\'') {
    return ScanString();
  }
  return ScanOperator();
}

Token Lexer::PeekToken() {
  if (!has_peeked_) {
    peeked_token_ = NextToken();
    has_peeked_ = true;
  }
  return peeked_token_;
}

}  // namespace raftsql
```

### `src/sql/ast.h`

```cpp
#pragma once

#include <memory>
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
  std::string agg_func;   // "COUNT" / "SUM" / "MIN" / "MAX"（空=非聚合）
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
```

### `src/sql/ast.cc`

```cpp
#include "src/sql/ast.h"

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

std::string StmtToString(const Stmt& stmt) {
  return std::visit([](const auto& s) -> std::string {
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
  }, stmt);
}

}  // namespace raftsql
```

### `src/sql/parser.h`

```cpp
#pragma once

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
  const std::string& GetError() const { return error_; }

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
  Token Expect(TokenType type);    // 期望并消费，不匹配则设 error
  bool Match(TokenType type);      // 尝试匹配并消费
  Token Peek();                    // 预看
  Token Advance();                 // 消费并返回
  void SetError(const std::string& msg);

  // 将标识符转大写（用于聚合函数识别）
  static std::string ToUpper(std::string s);

  Lexer lexer_;
  std::string error_;
  bool has_error_ = false;
};

}  // namespace raftsql
```

### `src/sql/parser.cc`

```cpp
#include "src/sql/parser.h"

#include <algorithm>
#include <cctype>

namespace raftsql {

Parser::Parser(std::string_view sql) : lexer_(sql) {}

std::string Parser::ToUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::toupper);
  return s;
}

Token Parser::Peek() { return lexer_.PeekToken(); }

Token Parser::Advance() { return lexer_.NextToken(); }

bool Parser::Match(TokenType type) {
  if (Peek().type == type) {
    Advance();
    return true;
  }
  return false;
}

Token Parser::Expect(TokenType type) {
  auto tok = Advance();
  if (tok.type != type) {
    SetError("Expected token type " + std::to_string(static_cast<int>(type)) +
             " but got '" + tok.value + "'");
    return {TokenType::kEof, ""};
  }
  return tok;
}

void Parser::SetError(const std::string &msg) {
  if (!has_error_) {
    error_ = msg;
    has_error_ = true;
  }
}

std::optional<Stmt> Parser::Parse() {
  auto tok = Peek();
  if (tok.type == TokenType::kSelect) {
    auto stmt = ParseSelect();
    if (!stmt)
      return std::nullopt;
    return Stmt{std::move(*stmt)};
  }
  if (tok.type == TokenType::kInsert) {
    auto stmt = ParseInsert();
    if (!stmt)
      return std::nullopt;
    return Stmt{std::move(*stmt)};
  }
  if (tok.type == TokenType::kCreate) {
    auto stmt = ParseCreate();
    if (!stmt)
      return std::nullopt;
    return Stmt{std::move(*stmt)};
  }
  if (tok.type == TokenType::kUpdate) {
    auto stmt = ParseUpdate();
    if (!stmt)
      return std::nullopt;
    return Stmt{std::move(*stmt)};
  }
  if (tok.type == TokenType::kDelete) {
    auto stmt = ParseDelete();
    if (!stmt)
      return std::nullopt;
    return Stmt{std::move(*stmt)};
  }
  SetError("Unknown statement starting with '" + tok.value + "'");
  return std::nullopt;
}

std::optional<SelectStmt> Parser::ParseSelect() {
  Expect(TokenType::kSelect);
  if (has_error_)
    return std::nullopt;

  SelectStmt stmt;

  auto next = Peek();
  // 检查是否为聚合函数
  if (next.type == TokenType::kIdent &&
      (ToUpper(next.value) == "COUNT" || ToUpper(next.value) == "SUM" ||
       ToUpper(next.value) == "MIN" || ToUpper(next.value) == "MAX")) {
    stmt.agg_func = ToUpper(Advance().value);
    Expect(TokenType::kLParen);
    if (has_error_)
      return std::nullopt;
    if (Match(TokenType::kStar)) {
      stmt.agg_column = "*";
    } else {
      stmt.agg_column = Expect(TokenType::kIdent).value;
    }
    Expect(TokenType::kRParen);
    if (has_error_)
      return std::nullopt;
  } else if (Match(TokenType::kStar)) {
    // SELECT * → columns 为空
  } else {
    // SELECT col1, col2, ...
    do {
      auto col = Expect(TokenType::kIdent);
      if (has_error_)
        return std::nullopt;
      stmt.columns.push_back(col.value);
    } while (Match(TokenType::kComma));
  }

  Expect(TokenType::kFrom);
  if (has_error_)
    return std::nullopt;

  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_)
    return std::nullopt;

  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_)
      return std::nullopt;
  }

  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<InsertStmt> Parser::ParseInsert() {
  Expect(TokenType::kInsert);
  Expect(TokenType::kInto);
  if (has_error_)
    return std::nullopt;

  InsertStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_)
    return std::nullopt;

  // 列名列表
  Expect(TokenType::kLParen);
  if (has_error_)
    return std::nullopt;
  do {
    auto col = Expect(TokenType::kIdent);
    if (has_error_)
      return std::nullopt;
    stmt.columns.push_back(col.value);
  } while (Match(TokenType::kComma));
  Expect(TokenType::kRParen);
  if (has_error_)
    return std::nullopt;

  Expect(TokenType::kValues);
  if (has_error_)
    return std::nullopt;

  // 值列表
  Expect(TokenType::kLParen);
  if (has_error_)
    return std::nullopt;
  do {
    auto tok = Peek();
    if (tok.type == TokenType::kNumber) {
      stmt.values.push_back(Advance().value);
    } else if (tok.type == TokenType::kString) {
      stmt.values.push_back(Advance().value);
    } else if (tok.type == TokenType::kIdent) {
      stmt.values.push_back(Advance().value);
    } else {
      SetError("Expected value in INSERT VALUES but got '" + tok.value + "'");
      return std::nullopt;
    }
  } while (Match(TokenType::kComma));
  Expect(TokenType::kRParen);
  if (has_error_)
    return std::nullopt;

  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<CreateTableStmt> Parser::ParseCreate() {
  Expect(TokenType::kCreate);
  Expect(TokenType::kTable);
  if (has_error_)
    return std::nullopt;

  CreateTableStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_)
    return std::nullopt;

  Expect(TokenType::kLParen);
  if (has_error_)
    return std::nullopt;

  while (!has_error_) {
    auto tok = Peek();
    // PRIMARY KEY(col) 子句
    if (tok.type == TokenType::kPrimary) {
      Advance();
      Expect(TokenType::kKey);
      Expect(TokenType::kLParen);
      auto pk_col = Expect(TokenType::kIdent).value;
      Expect(TokenType::kRParen);
      if (has_error_)
        return std::nullopt;
      // 标记主键列
      for (auto &col : stmt.columns) {
        if (col.name == pk_col) {
          col.primary_key = true;
        }
      }
    } else if (tok.type == TokenType::kIdent) {
      // 列定义: name type [constraint]
      ColumnSpec col;
      col.name = Advance().value;
      // 解析类型
      auto type_tok = Advance();
      if (type_tok.type == TokenType::kInt) {
        col.type = "INT";
      } else if (type_tok.type == TokenType::kVarchar) {
        // VARCHAR(N)
        col.type = "VARCHAR";
        if (Match(TokenType::kLParen)) {
          auto n = Expect(TokenType::kNumber);
          col.type = "VARCHAR(" + n.value + ")";
          Expect(TokenType::kRParen);
        }
      } else {
        // 其他类型按原文保留
        col.type = type_tok.value;
      }
      if (has_error_)
        return std::nullopt;
      // 支持内联 PRIMARY KEY：id INT PRIMARY KEY
      if (Peek().type == TokenType::kPrimary) {
        Advance();
        Expect(TokenType::kKey);
        col.primary_key = true;
        if (has_error_)
          return std::nullopt;
      }
      stmt.columns.push_back(std::move(col));
    } else {
      break;
    }

    if (!Match(TokenType::kComma))
      break;
    // 如果下一个是 ')' 则结束
    if (Peek().type == TokenType::kRParen)
      break;
  }

  Expect(TokenType::kRParen);
  if (has_error_)
    return std::nullopt;

  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<UpdateStmt> Parser::ParseUpdate() {
  Expect(TokenType::kUpdate);
  if (has_error_)
    return std::nullopt;

  UpdateStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_)
    return std::nullopt;

  Expect(TokenType::kSet);
  if (has_error_)
    return std::nullopt;

  // SET col1 = val1, col2 = val2
  do {
    auto col = Expect(TokenType::kIdent).value;
    if (has_error_)
      return std::nullopt;
    Expect(TokenType::kEq);
    if (has_error_)
      return std::nullopt;

    auto val_tok = Peek();
    std::string val;
    if (val_tok.type == TokenType::kNumber ||
        val_tok.type == TokenType::kString ||
        val_tok.type == TokenType::kIdent) {
      val = Advance().value;
    } else {
      SetError("Expected value in SET clause");
      return std::nullopt;
    }
    stmt.set_clauses.emplace_back(col, val);
  } while (Match(TokenType::kComma));

  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_)
      return std::nullopt;
  }

  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<DeleteStmt> Parser::ParseDelete() {
  Expect(TokenType::kDelete);
  Expect(TokenType::kFrom);
  if (has_error_)
    return std::nullopt;

  DeleteStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_)
    return std::nullopt;

  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_)
      return std::nullopt;
  }

  Match(TokenType::kSemicolon);
  return stmt;
}

std::unique_ptr<Expr> Parser::ParseOrExpr() {
  auto left = ParseAndExpr();
  if (!left)
    return nullptr;

  while (Peek().type == TokenType::kOr) {
    Advance();
    auto right = ParseAndExpr();
    if (!right)
      return nullptr;
    left = Expr::MakeBinOp("OR", std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParseAndExpr() {
  auto left = ParseCmpExpr();
  if (!left)
    return nullptr;

  while (Peek().type == TokenType::kAnd) {
    Advance();
    auto right = ParseCmpExpr();
    if (!right)
      return nullptr;
    left = Expr::MakeBinOp("AND", std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParseCmpExpr() {
  auto left = ParsePrimary();
  if (!left)
    return nullptr;

  auto tok = Peek();
  std::string op;
  if (tok.type == TokenType::kEq)
    op = "=";
  else if (tok.type == TokenType::kNeq)
    op = "!=";
  else if (tok.type == TokenType::kLt)
    op = "<";
  else if (tok.type == TokenType::kGt)
    op = ">";
  else if (tok.type == TokenType::kLe)
    op = "<=";
  else if (tok.type == TokenType::kGe)
    op = ">=";

  if (!op.empty()) {
    Advance();
    auto right = ParsePrimary();
    if (!right)
      return nullptr;
    return Expr::MakeBinOp(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParsePrimary() {
  auto tok = Peek();

  if (tok.type == TokenType::kLParen) {
    Advance();
    auto expr = ParseOrExpr();
    Expect(TokenType::kRParen);
    if (has_error_)
      return nullptr;
    return expr;
  }

  if (tok.type == TokenType::kIdent) {
    Advance();
    return Expr::MakeColumn(tok.value);
  }

  if (tok.type == TokenType::kNumber) {
    Advance();
    return Expr::MakeLiteral(tok.value);
  }

  if (tok.type == TokenType::kString) {
    Advance();
    return Expr::MakeLiteral(tok.value);
  }

  SetError("Unexpected token in expression: '" + tok.value + "'");
  return nullptr;
}

} // namespace raftsql
```

### `tests/unit/parser_test.cc`

```cpp
#include <gtest/gtest.h>

#include "src/sql/lexer.h"
#include "src/sql/parser.h"

namespace raftsql {

// ===== Lexer 测试 =====

TEST(LexerTest, BasicTokens) {
  Lexer lexer("SELECT * FROM t;");
  EXPECT_EQ(lexer.NextToken().type, TokenType::kSelect);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kStar);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kFrom);
  auto ident = lexer.NextToken();
  EXPECT_EQ(ident.type, TokenType::kIdent);
  EXPECT_EQ(ident.value, "t");
  EXPECT_EQ(lexer.NextToken().type, TokenType::kSemicolon);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kEof);
}

TEST(LexerTest, CaseInsensitiveKeywords) {
  Lexer lexer("select FROM where");
  EXPECT_EQ(lexer.NextToken().type, TokenType::kSelect);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kFrom);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kWhere);
}

TEST(LexerTest, Operators) {
  Lexer lexer("<= >= != = < >");
  EXPECT_EQ(lexer.NextToken().type, TokenType::kLe);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kGe);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kNeq);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kEq);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kLt);
  EXPECT_EQ(lexer.NextToken().type, TokenType::kGt);
}

TEST(LexerTest, StringLiteral) {
  Lexer lexer("'hello world'");
  auto tok = lexer.NextToken();
  EXPECT_EQ(tok.type, TokenType::kString);
  EXPECT_EQ(tok.value, "hello world");
}

// ===== Parser 测试 =====

TEST(ParserTest, SelectStar) {
  Parser parser("SELECT * FROM users");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  EXPECT_TRUE(stmt.columns.empty());       // * → columns 为空
  EXPECT_EQ(stmt.table_name, "users");
  EXPECT_EQ(stmt.where_expr, nullptr);
}

TEST(ParserTest, SelectColumns) {
  Parser parser("SELECT id, name FROM users");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  ASSERT_EQ(stmt.columns.size(), 2u);
  EXPECT_EQ(stmt.columns[0], "id");
  EXPECT_EQ(stmt.columns[1], "name");
}

TEST(ParserTest, SelectWhere) {
  Parser parser("SELECT * FROM users WHERE age > 18");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, ">");
  EXPECT_EQ(stmt.where_expr->left->col_name, "age");
  EXPECT_EQ(stmt.where_expr->right->literal, "18");
}

TEST(ParserTest, SelectWhereAnd) {
  Parser parser("SELECT * FROM t WHERE a > 1 AND b = 'x'");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, "AND");
}

TEST(ParserTest, Insert) {
  Parser parser("INSERT INTO users (id, name) VALUES (1, 'alice')");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<InsertStmt>(*result);
  EXPECT_EQ(stmt.table_name, "users");
  ASSERT_EQ(stmt.columns.size(), 2u);
  ASSERT_EQ(stmt.values.size(), 2u);
  EXPECT_EQ(stmt.values[0], "1");
  EXPECT_EQ(stmt.values[1], "alice");
}

TEST(ParserTest, CreateTable) {
  Parser parser(
      "CREATE TABLE users (id INT, name VARCHAR(255), PRIMARY KEY(id))");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<CreateTableStmt>(*result);
  EXPECT_EQ(stmt.table_name, "users");
  ASSERT_EQ(stmt.columns.size(), 2u);
  EXPECT_EQ(stmt.columns[0].name, "id");
  EXPECT_EQ(stmt.columns[0].type, "INT");
  EXPECT_TRUE(stmt.columns[0].primary_key);
  EXPECT_EQ(stmt.columns[1].name, "name");
  EXPECT_FALSE(stmt.columns[1].primary_key);
}

TEST(ParserTest, Update) {
  Parser parser("UPDATE users SET name = 'bob' WHERE id = 1");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<UpdateStmt>(*result);
  EXPECT_EQ(stmt.table_name, "users");
  ASSERT_EQ(stmt.set_clauses.size(), 1u);
  EXPECT_EQ(stmt.set_clauses[0].first, "name");
  EXPECT_EQ(stmt.set_clauses[0].second, "bob");
  ASSERT_NE(stmt.where_expr, nullptr);
}

TEST(ParserTest, Delete) {
  Parser parser("DELETE FROM users WHERE id = 1");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<DeleteStmt>(*result);
  EXPECT_EQ(stmt.table_name, "users");
  ASSERT_NE(stmt.where_expr, nullptr);
}

TEST(ParserTest, ErrorHandling) {
  Parser parser("SELECTT * FROM t");  // 拼写错误
  auto result = parser.Parse();
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(parser.GetError().empty());
}

TEST(ParserTest, NestedExpr) {
  Parser parser("SELECT * FROM t WHERE (a > 1 OR b < 2) AND c = 3");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, "AND");
  EXPECT_EQ(stmt.where_expr->left->op, "OR");
}

TEST(ParserTest, SelectCountStar) {
  Parser parser("SELECT COUNT(*) FROM users");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.agg_func, "COUNT");
  EXPECT_EQ(stmt.agg_column, "*");
}

TEST(ParserTest, SelectSumColumn) {
  Parser parser("SELECT SUM(age) FROM users WHERE age > 18");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.agg_func, "SUM");
  EXPECT_EQ(stmt.agg_column, "age");
  ASSERT_NE(stmt.where_expr, nullptr);
}

TEST(ParserTest, SelectWithSemicolon) {
  Parser parser("SELECT * FROM t;");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto& stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.table_name, "t");
}

}  // namespace raftsql
```