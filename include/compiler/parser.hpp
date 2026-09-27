#pragma once

#include "compiler/ast.hpp"
#include "compiler/lexer.hpp"

#include <string_view>
#include <vector>

namespace sysy {

/** Parsing result; consumers must reject a program whenever errors is nonempty. / 解析结果；errors
 * 非空时使用方必须拒绝程序。 */
struct ParseResult {
    Program program;
    std::vector<FrontendError> errors;
};

/** Parse a source buffer, including lexing and recoverable syntax errors. /
 * 解析源码，包含词法分析和可恢复的语法错误。 */
[[nodiscard]] ParseResult parse(std::string_view source);

/** Parse previously lexed tokens; token sequence must terminate with End. /
 * 解析已有词元；序列必须以 End 结尾。 */
[[nodiscard]] ParseResult parse_tokens(const LexResult& lexed);

} // namespace sysy
