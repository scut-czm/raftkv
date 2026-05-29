#include "src/sql/lexer.h"
// TODO(9.1): 实现 Lexer
// 详见 note/day9_impl.md § 完整代码实现
//
// ├── kKeywords 关键字映射表（unordered_map，大写 key）
// ├── TokenTypeName()
// ├── Lexer::SkipWhitespace()
// ├── Lexer::ScanIdentOrKeyword()  → 转大写查表
// ├── Lexer::ScanNumber()
// ├── Lexer::ScanString()          → 处理 '...' 单引号 + 转义
// ├── Lexer::ScanOperator()        → 单字符 & 双字符运算符
// ├── Lexer::LookupKeyword()
// ├── Lexer::NextToken()
// └── Lexer::PeekToken()

#include <algorithm>
#include <cctype>
#include <cstddef>
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

const char *TokenTypeName(TokenType type) {
  switch (type) {
  case TokenType::kSelect:
    return "SELECT";
  case TokenType::kFrom:
    return "FROM";
  case TokenType::kWhere:
    return "WHERE";
  case TokenType::kInsert:
    return "INSERT";
  case TokenType::kInto:
    return "INTO";
  case TokenType::kValues:
    return "VALUES";
  case TokenType::kCreate:
    return "CREATE";
  case TokenType::kTable:
    return "TABLE";
  case TokenType::kUpdate:
    return "UPDATE";
  case TokenType::kSet:
    return "SET";
  case TokenType::kDelete:
    return "DELETE";
  case TokenType::kInt:
    return "INT";
  case TokenType::kVarchar:
    return "VARCHAR";
  case TokenType::kPrimary:
    return "PRIMARY";
  case TokenType::kKey:
    return "KEY";
  case TokenType::kIdent:
    return "IDENT";
  case TokenType::kNumber:
    return "NUMBER";
  case TokenType::kString:
    return "STRING";
  case TokenType::kStar:
    return "STAR";
  case TokenType::kComma:
    return "COMMA";
  case TokenType::kLParen:
    return "LPAREN";
  case TokenType::kRParen:
    return "RPAREN";
  case TokenType::kSemicolon:
    return "SEMICOLON";
  case TokenType::kDot:
    return "DOT";
  case TokenType::kEq:
    return "EQ";
  case TokenType::kNeq:
    return "NEQ";
  case TokenType::kLt:
    return "LT";
  case TokenType::kGt:
    return "GT";
  case TokenType::kLe:
    return "LE";
  case TokenType::kGe:
    return "GE";
  case TokenType::kAnd:
    return "AND";
  case TokenType::kOr:
    return "OR";
  case TokenType::kEof:
    return "EOF";
  default:
    return "UNKNOWN";
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
    pos_++;
  }
  std::string ident(input_.substr(start, pos_ - start));
  TokenType type = LookupKeyword(ident);
  return {type, ident};
}

Token Lexer::ScanNumber() {
  size_t start = pos_;
  while (pos_ < input_.size() &&
         std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
    pos_++;
  }
  std::string ident(input_.substr(start, pos_ - start));
  return {TokenType::kNumber, ident};
}
Token Lexer::ScanString() {
  ++pos_; // skip opening '
  size_t start = pos_;
  std::string value;
  while (pos_ < input_.size() && input_[pos_] != '\'') {
    if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
      ++pos_; // skip backslash
      value += input_[pos_];
    } else {
      value += input_[pos_];
    }
    ++pos_;
  }
  if (pos_ < input_.size()) {
    ++pos_; // skip closing '
  }
  return {TokenType::kString, value};
}

Token Lexer::ScanOperator() {
  // 消费第一个字符
  char c = input_[pos_++];
  switch (c) {
  case '<':
    if (pos_ < input_.size()) {
      if (input_[pos_] == '=') {
        ++pos_;
        return {TokenType::kLe, "<="};
      }
      if (input_[pos_] == '>') {
        ++pos_;
        return {TokenType::kNeq, "<>"};
      }
      return {TokenType::kLt, "<"};
    }
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

  case '*':
    return {TokenType::kStar, "*"};
  case ',':
    return {TokenType::kComma, ","};
  case '(':
    return {TokenType::kLParen, "("};
  case ')':
    return {TokenType::kRParen, ")"};
  case ';':
    return {TokenType::kSemicolon, ";"};
  case '.':
    return {TokenType::kDot, "."};
  default:
    return {TokenType::kEof, ""};
  }
}

TokenType Lexer::LookupKeyword(const std::string &ident) {
  std::string upper = ident;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
  auto it = kKeywords.find(upper);
  if (it != kKeywords.end()) {
    return it->second;
  }
  return TokenType::kIdent;
}

Token Lexer::NextToken() {
  if (has_peeked_) {
    has_peeked_ = false;
    return peeked_token_;
  }
  SkipWhitespace();
  if (pos_ >= input_.size()) {
    return {TokenType::kEof, ""};
  }
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

} // namespace raftsql
