#include "compiler/parser.hpp"

#include <stdexcept>
#include <utility>

namespace sysy {
namespace {

struct ParseFailure {};

/** Recursive-descent parser with precedence climbing and declaration-level recovery. /
 * 递归下降解析器，采用优先级爬升及声明级错误恢复。 */
class Parser {
public:
    explicit Parser(const LexResult& lexed)
        : tokens_(lexed.tokens) {
        result_.errors = lexed.errors;
    }

    ParseResult
    run() {
        if (tokens_.empty() || tokens_.back().kind != TokenKind::End) {
            result_.errors.push_back({{}, "token stream must end with End"});
            return std::move(result_);
        }
        while (!is(TokenKind::End)) {
            const auto start = index_;
            try {
                result_.program.items.push_back(top_level());
            } catch (ParseFailure) {
                synchronize(false);
            }
            if (index_ == start)
                advance();
        }
        return std::move(result_);
    }

private:
    const std::vector<Token>& tokens_;
    std::size_t index_ = 0;
    ParseResult result_;

    const Token&
    current() const {
        return tokens_[index_];
    }

    const Token&
    previous() const {
        return tokens_[index_ - 1];
    }

    bool
    is(TokenKind kind) const {
        return current().kind == kind;
    }

    bool
    match(TokenKind kind) {
        if (!is(kind))
            return false;
        advance();
        return true;
    }

    void
    advance() {
        if (!is(TokenKind::End))
            ++index_;
    }

    [[noreturn]] void
    fail(std::string message) {
        result_.errors.push_back({current().range, std::move(message)});
        throw ParseFailure{};
    }

    const Token&
    expect(TokenKind kind, const char* message) {
        if (!is(kind))
            fail(message);
        const auto& token = current();
        advance();
        return token;
    }

    static bool
    type_start(TokenKind kind) {
        return kind == TokenKind::KwInt || kind == TokenKind::KwFloat;
    }

    BaseType
    base_type(bool allow_void) {
        if (match(TokenKind::KwInt))
            return BaseType::Int;
        if (match(TokenKind::KwFloat))
            return BaseType::Float;
        if (allow_void && match(TokenKind::KwVoid))
            return BaseType::Void;
        fail("expected type ('int' or 'float')");
    }

    void
    synchronize(bool block) {
        while (!is(TokenKind::End)) {
            if (match(TokenKind::Semicolon))
                return;
            if (block && is(TokenKind::RightBrace))
                return;
            if (is(TokenKind::KwConst)
                || type_start(current().kind)
                || is(TokenKind::KwVoid)
                || is(TokenKind::KwIf)
                || is(TokenKind::KwWhile)
                || is(TokenKind::KwReturn))
                return;
            advance();
        }
    }

    TopLevel
    top_level() {
        const auto begin = current().range.begin;
        TopLevel out;
        if (match(TokenKind::KwConst)) {
            out.declaration = declaration(true, begin);
        } else {
            const auto type = base_type(true);
            const auto& name = expect(TokenKind::Identifier, "expected identifier after type");
            if (is(TokenKind::LeftParen)) {
                out.function = function(type, name, begin);
            } else {
                if (type == BaseType::Void)
                    fail("void is only valid as a function return type");
                out.declaration = declaration_after_first(false, type, name, begin);
            }
        }
        out.range = {begin, previous().range.end};
        return out;
    }

    Declaration
    declaration(bool is_const, SourcePosition begin) {
        const auto type = base_type(false);
        const auto& name = expect(TokenKind::Identifier, "expected declared identifier");
        return declaration_after_first(is_const, type, name, begin);
    }

    Declaration
    declaration_after_first(bool is_const, BaseType type, const Token& name, SourcePosition begin) {
        Declaration out;
        out.is_const = is_const;
        out.type = type;
        out.definitions.push_back(definition(name, is_const));
        while (match(TokenKind::Comma)) {
            const auto& next = expect(TokenKind::Identifier, "expected identifier after ','");
            out.definitions.push_back(definition(next, is_const));
        }
        const auto& end = expect(TokenKind::Semicolon, "expected ';' after declaration");
        out.range = {begin, end.range.end};
        return out;
    }

    Definition
    definition(const Token& name, bool is_const) {
        Definition out;
        out.name = name.spelling;
        while (match(TokenKind::LeftBracket)) {
            out.dimensions.push_back(expression());
            expect(TokenKind::RightBracket, "expected ']' after array dimension");
        }
        if (match(TokenKind::Assign))
            out.initializer = initializer();
        else if (is_const)
            fail("constant declaration requires initializer");
        out.range = {name.range.begin, previous().range.end};
        return out;
    }

    Initializer
    initializer() {
        Initializer out;
        const auto begin = current().range.begin;
        if (match(TokenKind::LeftBrace)) {
            if (!is(TokenKind::RightBrace)) {
                do {
                    out.elements.push_back(initializer());
                } while (match(TokenKind::Comma));
            }
            const auto& end = expect(TokenKind::RightBrace, "expected '}' after initializer list");
            out.range = {begin, end.range.end};
        } else {
            out.expr = expression();
            out.range = out.expr->range;
        }
        return out;
    }

    Function
    function(BaseType type, const Token& name, SourcePosition begin) {
        Function out;
        out.return_type = type;
        out.name = name.spelling;
        expect(TokenKind::LeftParen, "expected '('");
        if (is(TokenKind::KwVoid) && tokens_[index_ + 1].kind == TokenKind::RightParen) {
            advance();
        } else if (!is(TokenKind::RightParen)) {
            do {
                out.parameters.push_back(parameter());
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RightParen, "expected ')' after parameters");
        if (match(TokenKind::Semicolon))
            out.range = {begin, previous().range.end};
        else {
            out.body = block();
            out.range = {begin, out.body->range.end};
        }
        return out;
    }

    Parameter
    parameter() {
        const auto begin = current().range.begin;
        Parameter out;
        out.type = base_type(false);
        if (is(TokenKind::Identifier)) {
            out.name = current().spelling;
            advance();
        }
        if (match(TokenKind::LeftBracket)) {
            out.is_array = true;
            expect(TokenKind::RightBracket, "array parameter's first dimension must be empty: []");
            while (match(TokenKind::LeftBracket)) {
                out.dimensions.push_back(expression());
                expect(TokenKind::RightBracket, "expected ']' after array parameter dimension");
            }
        }
        out.range = {begin, previous().range.end};
        return out;
    }

    StmtPtr
    block() {
        const auto& open = expect(TokenKind::LeftBrace, "expected '{' to begin block");
        auto out = std::make_shared<Statement>();
        out->kind = StmtKind::Block;
        while (!is(TokenKind::RightBrace) && !is(TokenKind::End)) {
            const auto start = index_;
            try {
                BlockItem item;
                if (match(TokenKind::KwConst))
                    item.declaration = declaration(true, previous().range.begin);
                else if (type_start(current().kind)) {
                    const auto begin = current().range.begin;
                    const auto type = base_type(false);
                    const auto& name =
                        expect(TokenKind::Identifier, "expected declared identifier");
                    item.declaration = declaration_after_first(false, type, name, begin);
                } else
                    item.statement = statement();
                item.range = item.declaration ? item.declaration->range : item.statement->range;
                out->items.push_back(std::move(item));
            } catch (ParseFailure) {
                synchronize(true);
            }
            if (index_ == start)
                advance();
        }
        const auto& close = expect(TokenKind::RightBrace, "expected '}' to close block");
        out->range = {open.range.begin, close.range.end};
        return out;
    }

    StmtPtr
    statement() {
        const auto begin = current().range.begin;
        if (is(TokenKind::LeftBrace))
            return block();
        auto out = std::make_shared<Statement>();
        if (match(TokenKind::KwIf)) {
            out->kind = StmtKind::If;
            expect(TokenKind::LeftParen, "expected '(' after if");
            out->expr = expression();
            expect(TokenKind::RightParen, "expected ')' after if condition");
            out->first = statement();
            if (match(TokenKind::KwElse))
                out->second = statement();
        } else if (match(TokenKind::KwWhile)) {
            out->kind = StmtKind::While;
            expect(TokenKind::LeftParen, "expected '(' after while");
            out->expr = expression();
            expect(TokenKind::RightParen, "expected ')' after while condition");
            out->first = statement();
        } else if (match(TokenKind::KwBreak)) {
            out->kind = StmtKind::Break;
            expect(TokenKind::Semicolon, "expected ';' after break");
        } else if (match(TokenKind::KwContinue)) {
            out->kind = StmtKind::Continue;
            expect(TokenKind::Semicolon, "expected ';' after continue");
        } else if (match(TokenKind::KwReturn)) {
            out->kind = StmtKind::Return;
            if (!is(TokenKind::Semicolon))
                out->expr = expression();
            expect(TokenKind::Semicolon, "expected ';' after return");
        } else if (match(TokenKind::Semicolon)) {
            out->kind = StmtKind::Expression;
        } else {
            auto left = expression();
            if (match(TokenKind::Assign)) {
                if (left->kind != ExprKind::Variable)
                    fail("assignment target must be a variable or array element");
                out->kind = StmtKind::Assignment;
                out->target = std::move(left);
                out->expr = expression();
            } else {
                out->kind = StmtKind::Expression;
                out->expr = std::move(left);
            }
            expect(TokenKind::Semicolon, "expected ';' after statement");
        }
        out->range = {begin, previous().range.end};
        return out;
    }

    static int
    precedence(TokenKind kind) {
        switch (kind) {
        case TokenKind::OrOr:
            return 1;
        case TokenKind::AndAnd:
            return 2;
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual:
            return 3;
        case TokenKind::Less:
        case TokenKind::Greater:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:
            return 4;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 5;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            return 6;
        default:
            return 0;
        }
    }

    ExprPtr
    expression(int min_precedence = 1) {
        auto left = unary();
        while (precedence(current().kind) >= min_precedence) {
            const auto op = current();
            const int prec = precedence(op.kind);
            advance();
            auto right = expression(prec + 1);
            auto expr = std::make_shared<Expr>();
            expr->kind = ExprKind::Binary;
            expr->range = {left->range.begin, right->range.end};
            expr->text = op.spelling;
            expr->children = {std::move(left), std::move(right)};
            left = std::move(expr);
        }
        return left;
    }

    ExprPtr
    unary() {
        if (is(TokenKind::Plus) || is(TokenKind::Minus) || is(TokenKind::Bang)) {
            const auto op = current();
            advance();
            auto operand = unary();
            auto out = std::make_shared<Expr>();
            out->kind = ExprKind::Unary;
            out->range = {op.range.begin, operand->range.end};
            out->text = op.spelling;
            out->children.push_back(std::move(operand));
            return out;
        }
        return primary();
    }

    ExprPtr
    primary() {
        if (match(TokenKind::LeftParen)) {
            const auto begin = previous().range.begin;
            auto out = expression();
            out->range.begin = begin;
            out->range.end =
                expect(TokenKind::RightParen, "expected ')' after expression").range.end;
            return out;
        }
        const auto token = current();
        auto out = std::make_shared<Expr>();
        out->range = token.range;
        out->text = token.spelling;
        if (match(TokenKind::Integer))
            out->kind = ExprKind::Integer;
        else if (match(TokenKind::Float))
            out->kind = ExprKind::Float;
        else if (match(TokenKind::String))
            out->kind = ExprKind::String;
        else if (match(TokenKind::Identifier)) {
            if (match(TokenKind::LeftParen)) {
                out->kind = ExprKind::Call;
                if (!is(TokenKind::RightParen)) {
                    do {
                        out->children.push_back(expression());
                    } while (match(TokenKind::Comma));
                }
                out->range.end =
                    expect(TokenKind::RightParen, "expected ')' after call arguments").range.end;
            } else {
                out->kind = ExprKind::Variable;
                while (match(TokenKind::LeftBracket)) {
                    out->children.push_back(expression());
                    out->range.end =
                        expect(TokenKind::RightBracket, "expected ']' after subscript").range.end;
                }
            }
        } else
            fail("expected expression");
        return out;
    }
};
} // namespace

ParseResult
parse_tokens(const LexResult& lexed) {
    return Parser(lexed).run();
}

ParseResult
parse(std::string_view source) {
    return parse_tokens(lex(source));
}
} // namespace sysy
