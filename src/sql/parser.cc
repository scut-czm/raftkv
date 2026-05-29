#include "src/sql/parser.h"
// TODO(9.3): 实现递归下降 Parser
// 详见 note/day9_impl.md § 完整代码实现
//
// Parse() 分派：
// ├── kSelect → ParseSelect()
// ├── kInsert → ParseInsert()
// ├── kCreate → ParseCreate()
// ├── kUpdate → ParseUpdate()
// └── kDelete → ParseDelete()
//
// WHERE 表达式优先级（低→高）：
//   ParseOrExpr → ParseAndExpr → ParseCmpExpr → ParsePrimary

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

void Parser::SetError(const std::string &msg) {
  if (!has_error_) {
    has_error_ = true;
    error_ = msg;
  }
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

std::optional<Stmt> Parser::Parse() {
  auto tok = Peek();
  if (tok.type == TokenType::kSelect) {
    auto stmt = ParseSelect();
    if (!stmt) {
      return std::nullopt;
    }
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

std::optional<DeleteStmt> Parser::ParseDelete() {
  Expect(TokenType::kDelete);
  Expect(TokenType::kFrom);

  if (has_error_) {
    return std::nullopt;
  }
  DeleteStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_) {
    return std::nullopt;
  }
  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_) {
      return std::nullopt;
    }
  }
  Match(TokenType::kSemicolon);
  return stmt;
}
std::optional<InsertStmt> Parser::ParseInsert() {
  Expect(TokenType::kInsert);
  Expect(TokenType::kInto);
  if (has_error_) {
    return std::nullopt;
  }

  InsertStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_) {
    return std::nullopt;
  }

  // 列名列表
  Expect(TokenType::kLParen);
  if (has_error_) {
    return std::nullopt;
  }
  do {
    auto col = Expect(TokenType::kIdent);
    if (has_error_) {
      return std::nullopt;
    }
    stmt.columns.push_back(col.value);
  } while (Match(TokenType::kComma));
  Expect(TokenType::kRParen);
  if (has_error_) {
    return std::nullopt;
  }
  Expect(TokenType::kValues);
  if (has_error_) {
    return std::nullopt;
  }
  // 值列表
  Expect(TokenType::kLParen);
  if (has_error_) {
    return std::nullopt;
  }
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
  if (has_error_) {
    return std::nullopt;
  }
  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<UpdateStmt> Parser::ParseUpdate() {
  Expect(TokenType::kUpdate);
  if (has_error_) {
    return std::nullopt;
  }
  UpdateStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_) {
    return std::nullopt;
  }
  Expect(TokenType::kSet);
  if (has_error_) {
    return std::nullopt;
  }

  // SET col1 = val1, col2 = val2
  do {
    auto col = Expect(TokenType::kIdent).value;
    if (has_error_) {
      return std::nullopt;
    }
    Expect(TokenType::kEq);
    if (has_error_) {
      return std::nullopt;
    }
    auto val_tok = Peek();
    std::string val;
    if (val_tok.type == TokenType::kNumber) {
      val = Advance().value;
    } else if (val_tok.type == TokenType::kString) {
      val = Advance().value;
    } else if (val_tok.type == TokenType::kIdent) {
      val = Advance().value;
    } else {
      SetError("Expected value in UPDATE but got '" + val_tok.value + "'");
      return std::nullopt;
    }
    stmt.set_clauses.emplace_back(col, val);
  } while (Match(TokenType::kComma));
  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_) {
      return std::nullopt;
    }
  }
  Match(TokenType::kSemicolon);
  return stmt;
}

std::unique_ptr<Expr> Parser::ParsePrimary() {
  auto tok = Peek();

  if (tok.type == TokenType::kLParen) {
    Advance();
    auto expr = ParseOrExpr();
    Expect(TokenType::kRParen);
    if (has_error_) {
      return nullptr;
    }
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

std::unique_ptr<Expr> Parser::ParseCmpExpr() {
  auto left = ParsePrimary();
  if (!left) {
    return nullptr;
  }
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
    if (!right) {
      return nullptr;
    }
    return Expr::MakeBinOp(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParseAndExpr() {
  auto left = ParseCmpExpr();
  if (!left) {
    return nullptr;
  }
  while (Peek().type == TokenType::kAnd) {
    Advance();
    auto right = ParseCmpExpr();
    if (!right) {
      return nullptr;
    }
    left = Expr::MakeBinOp("AND", std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParseOrExpr() {
  auto left = ParseAndExpr();
  if (!left) {
    return nullptr;
  }
  while (Peek().type == TokenType::kOr) {
    Advance();
    auto right = ParseAndExpr();

    if (!right) {
      return nullptr;
    }
    left = Expr::MakeBinOp("OR", std::move(left), std::move(right));
  }
  return left;
}

std::optional<SelectStmt> Parser::ParseSelect() {
  Expect(TokenType::kSelect);
  if (has_error_) {
    return std::nullopt;
  }
  SelectStmt stmt;
  auto next = Peek();
  // 检查是否为聚合函数
  if (next.type == TokenType::kIdent &&
      (ToUpper(next.value) == "COUNT" || ToUpper(next.value) == "SUM" ||
       ToUpper(next.value) == "MIN" || ToUpper(next.value) == "MAX")) {
    stmt.agg_func = ToUpper(Advance().value);
    Expect(TokenType::kLParen);
    if (has_error_) {
      return std::nullopt;
    }
    if (Match(TokenType::kStar)) {
      stmt.agg_column = "*";
    } else {
      stmt.agg_column = Expect(TokenType::kIdent).value;
    }
    Expect(TokenType::kRParen);
    if (has_error_) {
      return std::nullopt;
    }
  } else if (Match(TokenType::kStar)) {
    // SELECT * → columns 为空
  } else {
    // SELECT col1, col2, ...
    do {
      auto col = Expect(TokenType::kIdent);
      if (has_error_) {
        return std::nullopt;
      }
      stmt.columns.push_back(col.value);
    } while (Match(TokenType::kComma));
  }
  Expect(TokenType::kFrom);
  if (has_error_) {
    return std::nullopt;
  }

  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_) {
    return std::nullopt;
  }
  if (Match(TokenType::kWhere)) {
    stmt.where_expr = ParseOrExpr();
    if (has_error_) {
      return std::nullopt;
    }
  }
  Match(TokenType::kSemicolon);
  return stmt;
}

std::optional<CreateTableStmt> Parser::ParseCreate() {
  Expect(TokenType::kCreate);
  Expect(TokenType::kTable);
  if (has_error_) {
    return std::nullopt;
  }
  CreateTableStmt stmt;
  stmt.table_name = Expect(TokenType::kIdent).value;
  if (has_error_) {
    return std::nullopt;
  }
  Expect(TokenType::kLParen);
  if (has_error_) {
    return std::nullopt;
  }
  while (!has_error_) {
    auto tok = Peek();
    // PRIMARY KEY(col) 子句
    if (tok.type == TokenType::kPrimary) {
      Advance();
      Expect(TokenType::kKey);
      Expect(TokenType::kLParen);
      auto pk_col = Expect(TokenType::kIdent).value;
      Expect(TokenType::kRParen);
      if (has_error_) {
        return std::nullopt;
      }
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
      if (has_error_) {
        return std::nullopt;
      }
      // 支持内联 PRIMARY KEY：id INT PRIMARY KEY
      if (Peek().type == TokenType::kPrimary) {
        Advance();
        Expect(TokenType::kKey);
        col.primary_key = true;
        if (has_error_) {
          return std::nullopt;
        }
      }
      stmt.columns.push_back(std::move(col));
    } else {
      break;
    }
    if (!Match(TokenType::kComma)) {
      break;
    }
    // 如果下一个是 ')' 则结束
    if (Peek().type == TokenType::kRParen) {
      break;
    }
  }
  Expect(TokenType::kRParen);
  if (has_error_) {
    return std::nullopt;
  }
  Match(TokenType::kSemicolon);
  return stmt;
}

} // namespace raftsql