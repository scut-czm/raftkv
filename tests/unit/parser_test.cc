// TODO(9.4): 编写单元测试（≥ 13 个用例）
// 详见 note/day9_impl.md § 完整代码实现
//
// LexerTest::
// ├── BasicTokens             - 基本 Token 扫描
// ├── CaseInsensitiveKeywords - 关键字大小写不敏感
// ├── Operators               - 运算符（<=, >=, !=）
// └── StringLiteral           - 单引号字符串
//
// ParserTest::
// ├── SelectStar              - SELECT * FROM t
// ├── SelectColumns           - SELECT col1, col2 FROM t
// ├── SelectWhere             - WHERE col > value
// ├── SelectWhereAnd          - WHERE a > 1 AND b = 'x'
// ├── Insert                  - INSERT INTO ... VALUES ...
// ├── CreateTable             - CREATE TABLE ... PRIMARY KEY(...)
// ├── Update                  - UPDATE ... SET ... WHERE ...
// ├── Delete                  - DELETE FROM ... WHERE ...
// ├── ErrorHandling           - 语法错误 → nullopt + 错误信息
// ├── NestedExpr              - WHERE (a > 1 OR b < 2) AND c = 3
// ├── SelectCountStar         - SELECT COUNT(*) FROM t
// ├── SelectSumColumn         - SELECT SUM(col) FROM t WHERE ...
// └── SelectWithSemicolon     - SELECT * FROM t;

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
  auto &stmt = std::get<SelectStmt>(*result);
  EXPECT_TRUE(stmt.columns.empty()); // * → columns 为空
  EXPECT_EQ(stmt.table_name, "users");
  EXPECT_EQ(stmt.where_expr, nullptr);
}

TEST(ParserTest, SelectColumns) {
  Parser parser("SELECT id, name FROM users");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  ASSERT_EQ(stmt.columns.size(), 2u);
  EXPECT_EQ(stmt.columns[0], "id");
  EXPECT_EQ(stmt.columns[1], "name");
}

TEST(ParserTest, SelectWhere) {
  Parser parser("SELECT * FROM users WHERE age > 18");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, ">");
  EXPECT_EQ(stmt.where_expr->left->col_name, "age");
  EXPECT_EQ(stmt.where_expr->right->literal, "18");
}

TEST(ParserTest, SelectWhereAnd) {
  Parser parser("SELECT * FROM t WHERE a > 1 AND b = 'x'");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, "AND");
}

TEST(ParserTest, Insert) {
  Parser parser("INSERT INTO users (id, name) VALUES (1, 'alice')");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<InsertStmt>(*result);
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
  auto &stmt = std::get<CreateTableStmt>(*result);
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
  auto &stmt = std::get<UpdateStmt>(*result);
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
  auto &stmt = std::get<DeleteStmt>(*result);
  EXPECT_EQ(stmt.table_name, "users");
  ASSERT_NE(stmt.where_expr, nullptr);
}

TEST(ParserTest, ErrorHandling) {
  Parser parser("SELECTT * FROM t"); // 拼写错误
  auto result = parser.Parse();
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(parser.GetError().empty());
}

TEST(ParserTest, NestedExpr) {
  Parser parser("SELECT * FROM t WHERE (a > 1 OR b < 2) AND c = 3");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  ASSERT_NE(stmt.where_expr, nullptr);
  EXPECT_EQ(stmt.where_expr->op, "AND");
  EXPECT_EQ(stmt.where_expr->left->op, "OR");
}

TEST(ParserTest, SelectCountStar) {
  Parser parser("SELECT COUNT(*) FROM users");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.agg_func, "COUNT");
  EXPECT_EQ(stmt.agg_column, "*");
}

TEST(ParserTest, SelectSumColumn) {
  Parser parser("SELECT SUM(age) FROM users WHERE age > 18");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.agg_func, "SUM");
  EXPECT_EQ(stmt.agg_column, "age");
  ASSERT_NE(stmt.where_expr, nullptr);
}

TEST(ParserTest, SelectWithSemicolon) {
  Parser parser("SELECT * FROM t;");
  auto result = parser.Parse();
  ASSERT_TRUE(result.has_value());
  auto &stmt = std::get<SelectStmt>(*result);
  EXPECT_EQ(stmt.table_name, "t");
}

} // namespace raftsql