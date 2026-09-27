#pragma once

#include "compiler/ast.hpp"

#include <cstddef>
#include <cstdio>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sysy {

/** 中文：诊断严重性；英文：Severity of a human-facing diagnostic. */
enum class DiagnosticSeverity { Error, Warning, Note };

/** 中文：显式颜色策略优先于环境自动检测；英文：Explicit color policy overrides automatic environment detection. */
enum class ColorMode { Auto, Always, Never };

/**
 * 中文：检测 stdout 或 stderr 的 ANSI 彩色能力；Auto 遵循 NO_COLOR、CLICOLOR_FORCE、CLICOLOR、TERM 与 TTY。
 * Always/Never 是用户显式选项，优先于环境。Windows 控制台需要时会开启 VT 处理。
 * English: Detect ANSI color capability for stdout or stderr. Auto observes NO_COLOR, CLICOLOR_FORCE,
 * CLICOLOR, TERM, and TTY. Explicit Always/Never override the environment; Windows enables VT when needed.
 */
[[nodiscard]] bool terminal_color_enabled(ColorMode mode, std::FILE* stream);

/**
 * 中文：把不可信文本（例如命令行参数或数据库错误）转换为单行、不可注入终端的可显示文本。
 * 控制字符及无效 UTF-8 字节被替换为问号；有效 UTF-8 内容保持不变。
 * English: Convert untrusted text, such as argv or database errors, into a single-line terminal-safe
 * string. Control characters and malformed UTF-8 bytes become question marks; valid UTF-8 is preserved.
 */
[[nodiscard]] std::string escape_terminal_text(std::string_view text);

/** 中文：从属说明可指向另一处源码；英文：A subordinate explanation may identify another source range. */
struct DiagnosticNote {
    std::string message;
    std::optional<SourceRange> range;
};

/** 中文：机器无关的诊断数据，不含 ANSI 控制符；英文：Machine-independent diagnostic data without ANSI escapes. */
struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    std::optional<SourceRange> range;
    std::vector<DiagnosticNote> notes;
    std::string code;
};

/**
 * 中文：拥有源码内容与按字节索引的行表。源码位置采用半开区间和 UTF-8 字节偏移；不复制行文本。
 * English: Own source text and a byte-indexed line table. Locations use half-open UTF-8 byte offsets;
 * line views do not copy source text.
 */
class SourceFile {
public:
    SourceFile(std::string path, std::string text);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::size_t line_count() const noexcept { return line_starts_.size(); }

    /** 中文：返回从一开始计数的行的正文；越界返回空视图。英文：Return a one-based line without its terminator, or an empty view out of range. */
    [[nodiscard]] std::string_view line_text(std::size_t line) const noexcept;

    /** 中文：从字节偏移重建行列；越界偏移钳位到 EOF。英文：Recover a byte-based line/column; clamp out-of-range offsets to EOF. */
    [[nodiscard]] SourcePosition position(std::size_t offset) const noexcept;

private:
    std::string path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

/**
 * 中文：把诊断呈现到给定流，生产代码默认只写 stderr；注入流用于测试。每条诊断的源码摘录有长度上限，
 * 且会转义控制字符，防止源码向终端注入转义序列。
 * English: Render diagnostics to a stream, defaulting strictly to stderr in production; stream injection
 * supports tests. Excerpts are bounded and control characters sanitized to prevent terminal injection.
 *
 * @code
 * SourceFile source{"input.sy", "int main() { return x; }\n"};
 * DiagnosticEngine diagnostics{source};
 * diagnostics.error("undefined identifier 'x'", SourceRange{{20, 1, 21}, {21, 1, 22}});
 * @endcode
 */
class DiagnosticEngine {
public:
    explicit DiagnosticEngine(const SourceFile& source, ColorMode color = ColorMode::Auto);
    DiagnosticEngine(const SourceFile& source, std::ostream& output, ColorMode color = ColorMode::Auto);

    /** 中文：立即输出一条诊断并更新计数。英文：Emit a diagnostic immediately and update severity counts. */
    void emit(const Diagnostic& diagnostic);
    void error(std::string message, std::optional<SourceRange> range = std::nullopt);
    void warning(std::string message, std::optional<SourceRange> range = std::nullopt);

    [[nodiscard]] std::size_t error_count() const noexcept { return errors_; }
    [[nodiscard]] std::size_t warning_count() const noexcept { return warnings_; }
    [[nodiscard]] bool has_errors() const noexcept { return errors_ != 0; }
    [[nodiscard]] bool color_enabled() const noexcept { return color_enabled_; }

private:
    const SourceFile& source_;
    std::ostream& output_;
    bool color_enabled_ = false;
    std::size_t errors_ = 0;
    std::size_t warnings_ = 0;

    void render_excerpt(const SourceRange& range);
};

} // namespace sysy
