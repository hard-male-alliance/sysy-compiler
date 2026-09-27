#include "compiler/diagnostics.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sysy {
namespace {

constexpr std::size_t excerpt_width = 120;
constexpr std::size_t tab_width = 4;

struct Glyph {
    std::string text;
    std::size_t byte_begin;
    std::size_t byte_end;
    std::size_t cell_begin;
    std::size_t width;
};

/** 中文：只接受合法 UTF-8 前缀，坏字节单独替换。英文：Accept only valid UTF-8 prefixes and replace malformed bytes individually. */
std::pair<char32_t, std::size_t> decode(std::string_view s, std::size_t at) {
    const auto first = static_cast<unsigned char>(s[at]);
    if (first < 0x80) return {first, 1};
    std::size_t count = first >= 0xF0 && first <= 0xF4 ? 4 :
                        first >= 0xE0 && first <= 0xEF ? 3 :
                        first >= 0xC2 && first <= 0xDF ? 2 : 0;
    if (count == 0 || at + count > s.size()) return {0xFFFD, 1};
    char32_t value = first & (count == 4 ? 0x07 : count == 3 ? 0x0F : 0x1F);
    for (std::size_t i = 1; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(s[at + i]);
        if ((byte & 0xC0) != 0x80) return {0xFFFD, 1};
        value = (value << 6) | (byte & 0x3F);
    }
    if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
        (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
        return {0xFFFD, 1};
    return {value, count};
}

/** 中文：估算常见东亚字符终端宽度；英文：Approximate terminal width for common East Asian characters. */
std::size_t cell_width(char32_t value) {
    if ((value >= 0x0300 && value <= 0x036F) || (value >= 0x1AB0 && value <= 0x1AFF) ||
        (value >= 0x1DC0 && value <= 0x1DFF) || (value >= 0xFE20 && value <= 0xFE2F)) return 0;
    if (value >= 0x1100 && (value <= 0x115F || value == 0x2329 || value == 0x232A ||
        (value >= 0x2E80 && value <= 0xA4CF) || (value >= 0xAC00 && value <= 0xD7A3) ||
        (value >= 0xF900 && value <= 0xFAFF) || (value >= 0xFE10 && value <= 0xFE19) ||
        (value >= 0xFE30 && value <= 0xFE6F) || (value >= 0xFF00 && value <= 0xFF60) ||
        (value >= 0xFFE0 && value <= 0xFFE6))) return 2;
    return 1;
}

/** 中文：把行转换为不可注入终端的字形序列。英文：Convert a line to terminal-safe display glyphs. */
std::vector<Glyph> glyphs_for(std::string_view line) {
    std::vector<Glyph> glyphs;
    std::size_t cell = 0;
    for (std::size_t i = 0; i < line.size();) {
        const auto [value, count] = decode(line, i);
        Glyph glyph{{}, i, i + count, cell, 1};
        if (value == '\t') {
            glyph.width = tab_width - cell % tab_width;
            glyph.text.assign(glyph.width, ' ');
        } else if (value < 0x20 || value == 0x7F || (value >= 0x80 && value < 0xA0) || value == 0x1B) {
            glyph.text = "?";
        } else if (value == 0xFFFD) {
            glyph.text = "?";
        } else {
            glyph.width = cell_width(value);
            glyph.text = std::string(line.substr(i, count));
        }
        glyphs.push_back(std::move(glyph));
        cell += glyphs.back().width;
        i += count;
    }
    return glyphs;
}

/** 中文：清除非源码字段中的终端控制字符。英文：Remove terminal controls from non-source diagnostic fields. */
std::string safe_text_impl(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto [value, count] = decode(text, i);
        if (value < 0x20 || value == 0x7F || (value >= 0x80 && value < 0xA0) || value == 0xFFFD)
            result += '?';
        else
            result.append(text.substr(i, count));
        i += count;
    }
    return result;
}

std::size_t cell_at(const std::vector<Glyph>& glyphs, std::size_t byte, std::size_t total_bytes) {
    for (const auto& glyph : glyphs) {
        if (byte <= glyph.byte_begin) return glyph.cell_begin;
        if (byte < glyph.byte_end) return glyph.cell_begin;
    }
    (void)total_bytes;
    return glyphs.empty() ? 0 : glyphs.back().cell_begin + glyphs.back().width;
}

/** 中文：跨平台读取环境变量，避免 MSVC 对 getenv 的弃用警告。英文：Read environment variables portably without MSVC's getenv deprecation. */
std::optional<std::string> environment(std::string_view key) {
#if defined(_WIN32) && defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t length = 0;
    const std::string name(key);
    if (_dupenv_s(&raw, &length, name.c_str()) != 0 || raw == nullptr) return std::nullopt;
    std::string value(raw);
    std::free(raw);
    return value;
#else
    if (const char* value = std::getenv(std::string(key).c_str())) return std::string(value);
    return std::nullopt;
#endif
}

bool truthy(const std::optional<std::string>& value) { return value && !value->empty() && *value != "0"; }

bool terminal_supports_color(std::FILE* stream) {
#ifdef _WIN32
    if (!stream || !_isatty(_fileno(stream))) return false;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0 &&
        !SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    return true;
#else
    return stream && isatty(fileno(stream)) != 0;
#endif
}

std::string_view label(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error: return "error";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Note: return "note";
    }
    return "error";
}

std::string_view color_code(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error: return "\x1b[1;31m";
    case DiagnosticSeverity::Warning: return "\x1b[1;33m";
    case DiagnosticSeverity::Note: return "\x1b[1;36m";
    }
    return "\x1b[1;31m";
}

} // namespace

std::string escape_terminal_text(std::string_view text) { return safe_text_impl(text); }

bool terminal_color_enabled(ColorMode mode, std::FILE* stream) {
    if (mode == ColorMode::Never) return false;
    if (mode == ColorMode::Always) return true;
    if (!stream) return false;
    if (environment("NO_COLOR")) return false;
    if (truthy(environment("CLICOLOR_FORCE"))) return true;
    if (environment("CLICOLOR") == "0") return false;
    if (environment("TERM") == "dumb") return false;
    return terminal_supports_color(stream);
}

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)), line_starts_{0} {
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
        else if (text_[i] == '\r') {
            if (i + 1 < text_.size() && text_[i + 1] == '\n') ++i;
            line_starts_.push_back(i + 1);
        }
    }
}

std::string_view SourceFile::line_text(std::size_t line) const noexcept {
    if (line == 0 || line > line_starts_.size()) return {};
    const auto begin = line_starts_[line - 1];
    auto end = line < line_starts_.size() ? line_starts_[line] : text_.size();
    while (end > begin && (text_[end - 1] == '\r' || text_[end - 1] == '\n')) --end;
    return std::string_view(text_).substr(begin, end - begin);
}

SourcePosition SourceFile::position(std::size_t offset) const noexcept {
    offset = std::min(offset, text_.size());
    const auto found = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto line = static_cast<std::size_t>(found - line_starts_.begin());
    return {offset, line, offset - line_starts_[line - 1] + 1};
}

DiagnosticEngine::DiagnosticEngine(const SourceFile& source, ColorMode color)
    : DiagnosticEngine(source, std::cerr, color) {}

DiagnosticEngine::DiagnosticEngine(const SourceFile& source, std::ostream& output, ColorMode color)
    : source_(source), output_(output),
      color_enabled_(terminal_color_enabled(color, &output == &std::cerr ? stderr : nullptr)) {}

void DiagnosticEngine::emit(const Diagnostic& diagnostic) {
    if (diagnostic.severity == DiagnosticSeverity::Error) ++errors_;
    else if (diagnostic.severity == DiagnosticSeverity::Warning) ++warnings_;

    if (diagnostic.range) {
        const auto where = source_.position(diagnostic.range->begin.offset);
        output_ << escape_terminal_text(source_.path()) << ':' << where.line << ':' << where.column << ": ";
    }
    if (color_enabled_) output_ << color_code(diagnostic.severity);
    output_ << label(diagnostic.severity);
    if (color_enabled_) output_ << "\x1b[0m";
    if (!diagnostic.code.empty()) output_ << '[' << escape_terminal_text(diagnostic.code) << ']';
    output_ << ": " << escape_terminal_text(diagnostic.message) << '\n';
    if (diagnostic.range) render_excerpt(*diagnostic.range);
    for (const auto& note : diagnostic.notes) {
        output_ << "  = ";
        if (color_enabled_) output_ << color_code(DiagnosticSeverity::Note);
        output_ << "note";
        if (color_enabled_) output_ << "\x1b[0m";
        output_ << ": " << escape_terminal_text(note.message);
        if (note.range) {
            const auto where = source_.position(note.range->begin.offset);
            output_ << " (" << escape_terminal_text(source_.path()) << ':' << where.line << ':' << where.column << ')';
        }
        output_ << '\n';
        if (note.range) render_excerpt(*note.range);
    }
    output_.flush();
}

void DiagnosticEngine::error(std::string message, std::optional<SourceRange> range) {
    emit({DiagnosticSeverity::Error, std::move(message), range, {}, {}});
}

void DiagnosticEngine::warning(std::string message, std::optional<SourceRange> range) {
    emit({DiagnosticSeverity::Warning, std::move(message), range, {}, {}});
}

void DiagnosticEngine::render_excerpt(const SourceRange& range) {
    if (range.begin.offset > source_.text().size()) return;
    const auto begin = source_.position(range.begin.offset);
    const auto end = source_.position(std::max(range.begin.offset, range.end.offset));
    const auto line = source_.line_text(begin.line);
    const auto glyphs = glyphs_for(line);
    const auto line_start = begin.offset - (begin.column - 1);
    const auto mark_byte = begin.offset - line_start;
    const auto last_byte = end.line == begin.line ? end.offset - line_start : line.size();
    const auto mark = cell_at(glyphs, mark_byte, line.size());
    const auto mark_end = cell_at(glyphs, std::min(last_byte, line.size()), line.size());
    const auto window = mark > 32 ? mark - 32 : 0;
    std::string visible;
    std::size_t displayed_start = 0;
    std::size_t displayed_end = 0;
    bool prefix = false;
    bool suffix = false;
    for (const auto& glyph : glyphs) {
        if (glyph.cell_begin + glyph.width <= window) { prefix = true; continue; }
        if (glyph.cell_begin >= window + excerpt_width) { suffix = true; break; }
        if (visible.empty()) displayed_start = glyph.cell_begin;
        displayed_end = glyph.cell_begin + glyph.width;
        visible += glyph.text;
    }
    if (glyphs.empty()) displayed_start = displayed_end = 0;
    const auto line_number = std::to_string(begin.line);
    const std::string gutter(line_number.size(), ' ');
    output_ << ' ' << line_number << " | " << (prefix ? "…" : "") << visible << (suffix ? "…" : "") << '\n';
    const auto relative = mark > displayed_start ? mark - displayed_start : 0;
    const auto available = displayed_end > mark ? displayed_end - mark : 1;
    const auto width = std::max<std::size_t>(1, std::min({mark_end > mark ? mark_end - mark : 1, available, excerpt_width}));
    output_ << ' ' << gutter << " | " << (prefix ? " " : "") << std::string(relative, ' ');
    if (color_enabled_) output_ << "\x1b[1;32m";
    output_ << '^' << std::string(width - 1, '~');
    if (color_enabled_) output_ << "\x1b[0m";
    output_ << '\n';
    if (end.line > begin.line) output_ << "  = note: span continues for " << end.line - begin.line << " more line(s)\n";
}

} // namespace sysy
