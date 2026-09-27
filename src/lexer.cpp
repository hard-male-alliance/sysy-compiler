#include "compiler/lexer.hpp"

#include <cctype>
#include <string_view>

namespace sysy {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_part(char c) { return ident_start(c) || digit(c); }

/** Stateful scanner; every cursor advancement updates byte and display coordinates. / 有状态扫描器，每次前进均更新字节及显示坐标。 */
class Scanner {
public:
    explicit Scanner(std::string_view input) : input_(input) {}

    LexResult run() {
        while (!at_end()) {
            skip_trivia();
            if (at_end()) break;
            const auto begin = pos_;
            const char c = peek();
            if (ident_start(c)) identifier(begin);
            else if (digit(c) || (c == '.' && digit(peek(1)))) number(begin);
            else if (c == '"') string(begin);
            else punctuation(begin);
        }
        result_.tokens.push_back({TokenKind::End, {pos_, pos_}, ""});
        return std::move(result_);
    }

private:
    std::string_view input_;
    SourcePosition pos_{};
    LexResult result_;

    bool at_end() const { return pos_.offset >= input_.size(); }
    char peek(std::size_t ahead = 0) const {
        const auto i = pos_.offset + ahead;
        return i < input_.size() ? input_[i] : '\0';
    }
    char take() {
        const char c = input_[pos_.offset++];
        if (c == '\n') { ++pos_.line; pos_.column = 1; }
        else ++pos_.column;
        return c;
    }
    void emit(TokenKind kind, SourcePosition begin) {
        result_.tokens.push_back({kind, {begin, pos_}, std::string(input_.substr(begin.offset, pos_.offset - begin.offset))});
    }
    void error(SourcePosition begin, std::string message) {
        result_.errors.push_back({{begin, pos_}, std::move(message)});
    }
    void skip_trivia() {
        for (;;) {
            while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n' || peek() == '\f' || peek() == '\v')) take();
            if (peek() == '/' && peek(1) == '/') {
                take(); take();
                while (!at_end() && peek() != '\n') take();
            } else if (peek() == '/' && peek(1) == '*') {
                const auto begin = pos_;
                take(); take();
                bool closed = false;
                while (!at_end()) {
                    if (peek() == '*' && peek(1) == '/') { take(); take(); closed = true; break; }
                    take();
                }
                if (!closed) error(begin, "unterminated block comment");
            } else break;
        }
    }
    void identifier(SourcePosition begin) {
        do { take(); } while (ident_part(peek()));
        const auto word = input_.substr(begin.offset, pos_.offset - begin.offset);
        TokenKind kind = TokenKind::Identifier;
        if (word == "int") kind = TokenKind::KwInt;
        else if (word == "float") kind = TokenKind::KwFloat;
        else if (word == "void") kind = TokenKind::KwVoid;
        else if (word == "const") kind = TokenKind::KwConst;
        else if (word == "if") kind = TokenKind::KwIf;
        else if (word == "else") kind = TokenKind::KwElse;
        else if (word == "while") kind = TokenKind::KwWhile;
        else if (word == "break") kind = TokenKind::KwBreak;
        else if (word == "continue") kind = TokenKind::KwContinue;
        else if (word == "return") kind = TokenKind::KwReturn;
        emit(kind, begin);
    }
    void malformed_number(SourcePosition begin, std::string message) {
        while (ident_part(peek()) || peek() == '.') take();
        emit(TokenKind::Invalid, begin);
        error(begin, std::move(message));
    }
    void exponent(bool binary, SourcePosition begin) {
        take();
        if (peek() == '+' || peek() == '-') take();
        if (!digit(peek())) { malformed_number(begin, binary ? "hexadecimal float requires exponent digits" : "float requires exponent digits"); return; }
        while (digit(peek())) take();
    }
    void number(SourcePosition begin) {
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            take(); take();
            bool any = false;
            while (hex(peek())) { take(); any = true; }
            bool dot = false;
            if (peek() == '.') {
                dot = true; take();
                while (hex(peek())) { take(); any = true; }
            }
            if (!any) { malformed_number(begin, "hexadecimal literal requires a hexadecimal digit"); return; }
            if (peek() == 'p' || peek() == 'P') {
                exponent(true, begin);
                if (result_.tokens.size() && result_.tokens.back().range.begin.offset == begin.offset) return;
                if (ident_part(peek()) || peek() == '.') { malformed_number(begin, "invalid hexadecimal float suffix"); return; }
                emit(TokenKind::Float, begin); return;
            }
            if (dot) { malformed_number(begin, "hexadecimal float requires a p exponent"); return; }
            if (ident_part(peek()) || peek() == '.') { malformed_number(begin, "invalid hexadecimal integer suffix"); return; }
            emit(TokenKind::Integer, begin); return;
        }
        bool dot = false, exp = false, bad_octal = false;
        if (peek() == '.') { dot = true; take(); }
        else {
            if (peek() == '0') {
                take();
                while (digit(peek())) { if (peek() >= '8') bad_octal = true; take(); }
            } else while (digit(peek())) take();
            if (peek() == '.') { dot = true; take(); }
        }
        while (digit(peek())) take();
        if (peek() == 'e' || peek() == 'E') {
            exp = true; exponent(false, begin);
            if (result_.tokens.size() && result_.tokens.back().range.begin.offset == begin.offset) return;
        }
        if (ident_part(peek()) || peek() == '.') { malformed_number(begin, "invalid numeric literal suffix"); return; }
        if (bad_octal && !dot && !exp) { emit(TokenKind::Invalid, begin); error(begin, "invalid octal digit (8 or 9)"); return; }
        emit(dot || exp ? TokenKind::Float : TokenKind::Integer, begin);
    }
    void string(SourcePosition begin) {
        take();
        bool closed = false;
        while (!at_end() && peek() != '\n') {
            if (peek() == '"') { take(); closed = true; break; }
            if (peek() == '\\') {
                take();
                if (!at_end() && peek() != '\n') take();
            } else take();
        }
        emit(closed ? TokenKind::String : TokenKind::Invalid, begin);
        if (!closed) error(begin, "unterminated string literal");
    }
    void punctuation(SourcePosition begin) {
        const char c = take();
        TokenKind kind = TokenKind::Invalid;
        switch (c) {
        case '+': kind = TokenKind::Plus; break;
        case '-': kind = TokenKind::Minus; break;
        case '*': kind = TokenKind::Star; break;
        case '/': kind = TokenKind::Slash; break;
        case '%': kind = TokenKind::Percent; break;
        case '!': kind = peek() == '=' ? (take(), TokenKind::BangEqual) : TokenKind::Bang; break;
        case '<': kind = peek() == '=' ? (take(), TokenKind::LessEqual) : TokenKind::Less; break;
        case '>': kind = peek() == '=' ? (take(), TokenKind::GreaterEqual) : TokenKind::Greater; break;
        case '=': kind = peek() == '=' ? (take(), TokenKind::EqualEqual) : TokenKind::Assign; break;
        case '&': if (peek() == '&') { take(); kind = TokenKind::AndAnd; } break;
        case '|': if (peek() == '|') { take(); kind = TokenKind::OrOr; } break;
        case ',': kind = TokenKind::Comma; break;
        case ';': kind = TokenKind::Semicolon; break;
        case '(': kind = TokenKind::LeftParen; break;
        case ')': kind = TokenKind::RightParen; break;
        case '[': kind = TokenKind::LeftBracket; break;
        case ']': kind = TokenKind::RightBracket; break;
        case '{': kind = TokenKind::LeftBrace; break;
        case '}': kind = TokenKind::RightBrace; break;
        default: break;
        }
        emit(kind, begin);
        if (kind == TokenKind::Invalid) error(begin, "unexpected character");
    }
};
} // namespace

LexResult lex(std::string_view source) { return Scanner(source).run(); }
} // namespace sysy
