#pragma once

#include "compiler/ast.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace sysy {

/** Token vocabulary for the SysY 2022 grammar. / SysY 2022 文法的词元种类。 */
enum class TokenKind {
    End, Invalid, Identifier, Integer, Float, String,
    KwInt, KwFloat, KwVoid, KwConst, KwIf, KwElse, KwWhile,
    KwBreak, KwContinue, KwReturn,
    Plus, Minus, Star, Slash, Percent, Bang,
    Less, Greater, LessEqual, GreaterEqual, EqualEqual, BangEqual,
    AndAnd, OrOr, Assign, Comma, Semicolon,
    LeftParen, RightParen, LeftBracket, RightBracket, LeftBrace, RightBrace
};

/** A lossless token; spelling retains numeric base and escape syntax. / 无损词元，spelling 保留数值进制及转义写法。 */
struct Token {
    TokenKind kind = TokenKind::Invalid;
    SourceRange range;
    std::string spelling;
};

/** Lexing result with nonfatal errors and a mandatory End sentinel. / 词法分析结果包含非致命错误及必有的 End 哨兵。 */
struct LexResult {
    std::vector<Token> tokens;
    std::vector<FrontendError> errors;
};

/** Tokenize a complete SysY source buffer; offsets refer to the original bytes. / 对完整 SysY 源缓冲区分词，偏移指向原始字节。 */
[[nodiscard]] LexResult lex(std::string_view source);

} // namespace sysy
