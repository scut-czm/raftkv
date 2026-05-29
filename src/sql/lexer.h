#pragma once
// TODO(9.1): 定义 Token 与 Lexer
// 详见 note/day9_impl.md § 完整代码实现
//
// ├── TokenType 枚举（关键字 / 标识符 / 符号 / 运算符 / EOF）
// ├── const char* TokenTypeName(TokenType type)
// ├── struct Token { TokenType type; std::string value; }
// └── class Lexer
//     ├── explicit Lexer(std::string_view input)
//     ├── Token NextToken()   // 消费并返回下一个 Token
//     └── Token PeekToken()   // 预看下一个 Token（不消费）

#include <string>
#include <string_view>

namespace raftsql {
enum class TokenType {
  // 关键字
  kSelect,  // SELECT
  kFrom,    // FROM
  kWhere,   // WHERE
  kInsert,  // INSERT
  kInto,    // INTO
  kValues,  // VALUES
  kCreate,  // CREATE
  kTable,   // TABLE
  kUpdate,  // UPDATE
  kSet,     // SET
  kDelete,  // DELETE
  kInt,     // INT（列类型）
  kVarchar, // VARCHAR（列类型）
  kPrimary, // PRIMARY（主键约束前缀）
  kKey,     // KEY（与 PRIMARY 合用：PRIMARY KEY）
  // 标识符与字面量
  kIdent,  // 用户定义名称，如表名、列名
  kNumber, // 整数字面量，如 42
  kString, // 单引号字符串，如 'Alice'
  // 符号
  kStar,      // *（SELECT * 或 COUNT(*)）
  kComma,     // ,（列表分隔符）
  kLParen,    // (（左括号）
  kRParen,    // )（右括号）
  kSemicolon, // ;（语句结束符）
  kDot,       // .（表名.列名分隔符）
  // 比较运算符
  kEq,  // =
  kNeq, // != 或 <>
  kLt,  // <
  kGt,  // >
  kLe,  // <=
  kGe,  // >=
  // 逻辑运算符
  kAnd, // AND
  kOr,  // OR
  // 结束
  kEof // 输入已耗尽
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
  bool has_peeked_ = false; // 缓冲区是否有token
  Token peeked_token_;
};
} // namespace raftsql
