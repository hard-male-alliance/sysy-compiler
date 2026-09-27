#include "compiler/diagnostics.hpp"
#include "compiler/lower.hpp"
#include "compiler/optimize.hpp"
#include "compiler/parser.hpp"
#include "compiler/riscv.hpp"
#include "compiler/semantic.hpp"
#include "compiler/telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/** 中文：稳定的单文件命令行配置；英文：Stable single-source command-line configuration. */
struct Options {
    std::string input;
    std::string output;
    std::string database;
    sysy::ColorMode color = sysy::ColorMode::Auto;
    int optimization = 0;
    bool summary = false;
    bool help = false;
    bool version = false;
    sysy::TraceContext remote_parent;
};

/** 中文：一次流水线阶段的墙钟耗时；英文：Wall time for one compiler pipeline stage. */
struct StageTime {
    std::string name;
    std::chrono::nanoseconds duration;
};

/** 中文：检查固定长度十六进制字段；英文：Validate a fixed-width hexadecimal field. */
bool is_hex(std::string_view text, std::size_t width) {
    return text.size() == width &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F');
           });
}

/** 中文：解析 W3C traceparent，避免把不可信字段写入追踪关系。英文：Parse W3C traceparent before persisting relationships. */
std::optional<sysy::TraceContext> parse_traceparent(std::string_view value) {
    if (value.size() != 55 || value[2] != '-' || value[35] != '-' || value[52] != '-' ||
        !is_hex(value.substr(0, 2), 2) || !is_hex(value.substr(3, 32), 32) ||
        !is_hex(value.substr(36, 16), 16) || !is_hex(value.substr(53, 2), 2) ||
        value.substr(0, 2) != "00" || value.substr(3, 32) == std::string(32, '0') ||
        value.substr(36, 16) == std::string(16, '0')) {
        return std::nullopt;
    }
    sysy::TraceContext context{std::string(value.substr(3, 32)), std::string(value.substr(36, 16))};
    const auto lowercase = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(context.trace_id.begin(), context.trace_id.end(), context.trace_id.begin(), lowercase);
    std::transform(context.span_id.begin(), context.span_id.end(), context.span_id.begin(), lowercase);
    return context;
}

/** 中文：从选项读取一个参数；英文：Consume a required option argument. */
std::optional<std::string> take_argument(int& index, int argc, char* argv[],
                                         std::string_view option, std::string& error) {
    if (index + 1 >= argc) {
        error = std::string(option) + " requires a value";
        return std::nullopt;
    }
    return std::string(argv[++index]);
}

/** 中文：识别相同文件的词法、规范化及现存硬链接别名，保护输入、输出和数据库。英文：Detect lexical, canonical, and existing hard-link aliases among source, artifact, and database. */
bool paths_alias(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty() || left == "-" || right == "-") return false;
    std::error_code error;
    const auto lhs = std::filesystem::absolute(std::filesystem::path(left), error);
    if (error) return left == right;
    error.clear();
    const auto rhs = std::filesystem::absolute(std::filesystem::path(right), error);
    if (error) return left == right;
    if (lhs.lexically_normal() == rhs.lexically_normal()) return true;
    error.clear();
    const auto lhs_canonical = std::filesystem::weakly_canonical(lhs, error);
    if (error) return false;
    error.clear();
    const auto rhs_canonical = std::filesystem::weakly_canonical(rhs, error);
    if (!error && lhs_canonical == rhs_canonical) return true;
    error.clear();
    return std::filesystem::exists(lhs, error) && !error &&
           std::filesystem::exists(rhs, error) && !error &&
           std::filesystem::equivalent(lhs, rhs, error) && !error;
}

/** 中文：解析用户命令行，拒绝歧义与重复输入；英文：Parse the CLI and reject ambiguous or repeated input. */
std::optional<Options> parse_options(int argc, char* argv[], std::string& error) {
    Options options;
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        if (!positional_only && arg == "--") {
            positional_only = true;
        } else if (!positional_only && (arg == "--help" || arg == "-h")) {
            options.help = true;
        } else if (!positional_only && arg == "--version") {
            options.version = true;
        } else if (!positional_only && (arg == "-S" || arg == "--summary")) {
            if (arg == "--summary") options.summary = true;
        } else if (!positional_only && (arg == "-O0" || arg == "-O1" || arg == "-O2")) {
            options.optimization = arg[2] - '0';
        } else if (!positional_only && (arg == "-o" || arg == "--output")) {
            auto value = take_argument(index, argc, argv, arg, error);
            if (!value) return std::nullopt;
            options.output = std::move(*value);
        } else if (!positional_only && arg == "--db") {
            auto value = take_argument(index, argc, argv, arg, error);
            if (!value) return std::nullopt;
            if (value->empty()) {
                error = "--db requires a nonempty path";
                return std::nullopt;
            }
            options.database = std::move(*value);
        } else if (!positional_only && arg.starts_with("--db=")) {
            options.database = std::string(arg.substr(5));
            if (options.database.empty()) {
                error = "--db requires a nonempty path";
                return std::nullopt;
            }
        } else if (!positional_only && arg == "--color") {
            auto value = take_argument(index, argc, argv, arg, error);
            if (!value) return std::nullopt;
            if (*value == "auto") options.color = sysy::ColorMode::Auto;
            else if (*value == "always") options.color = sysy::ColorMode::Always;
            else if (*value == "never") options.color = sysy::ColorMode::Never;
            else error = "--color expects auto, always, or never";
            if (!error.empty()) return std::nullopt;
        } else if (!positional_only && arg.starts_with("--color=")) {
            const auto value = arg.substr(8);
            if (value == "auto") options.color = sysy::ColorMode::Auto;
            else if (value == "always") options.color = sysy::ColorMode::Always;
            else if (value == "never") options.color = sysy::ColorMode::Never;
            else {
                error = "--color expects auto, always, or never";
                return std::nullopt;
            }
        } else if (!positional_only && arg == "--traceparent") {
            auto value = take_argument(index, argc, argv, arg, error);
            if (!value) return std::nullopt;
            auto parent = parse_traceparent(*value);
            if (!parent) {
                error = "--traceparent expects a valid W3C version 00 traceparent";
                return std::nullopt;
            }
            options.remote_parent = std::move(*parent);
        } else if (!positional_only && arg.starts_with('-') && arg != "-") {
            error = "unknown option '" + std::string(arg) + "'";
            return std::nullopt;
        } else if (!options.input.empty()) {
            error = "exactly one SysY source file is supported";
            return std::nullopt;
        } else {
            options.input = std::string(arg);
        }
    }
    if (options.help || options.version) return options;
    if (options.input.empty()) {
        error = "missing input .sy source file";
        return std::nullopt;
    }
    if (options.output.empty()) {
        if (options.input == "-") options.output = "-";
        else {
            auto path = std::filesystem::path(options.input);
            path.replace_extension(".s");
            options.output = path.string();
        }
    }
    if (options.output.empty()) {
        error = "output path must not be empty";
        return std::nullopt;
    }
    if (paths_alias(options.input, options.output)) {
        error = "output path must differ from input path";
        return std::nullopt;
    }
    if (paths_alias(options.database, options.input) || paths_alias(options.database, options.output)) {
        error = "SQLite database path must differ from input and output paths";
        return std::nullopt;
    }
    for (std::string_view suffix : {"-wal", "-shm", "-journal"}) {
        if (options.database.empty()) break;
        const auto sidecar = options.database + std::string(suffix);
        if (paths_alias(sidecar, options.input) || paths_alias(sidecar, options.output)) {
            error = "input and output paths must not alias SQLite sidecar files";
            return std::nullopt;
        }
    }
    return options;
}

/** 中文：人类帮助文本始终使用 stderr，给汇编 stdout 留出清晰边界。英文：Keep human help on stderr, reserving stdout for assembly. */
void print_help(sysy::ColorMode mode) {
    const bool color = sysy::terminal_color_enabled(mode, stderr);
    const auto bold = color ? "\033[1m" : "";
    const auto cyan = color ? "\033[36m" : "";
    const auto reset = color ? "\033[0m" : "";
    std::cerr << bold << "SysY compiler" << reset << "  •  RV64GC / LP64D GNU/Linux\n\n"
              << cyan << "Usage" << reset << "\n"
              << "  compiler [options] input.sy [-o output.s]\n"
              << "  compiler [options] - -o -    # stdin to assembly stdout\n\n"
              << cyan << "Options" << reset << "\n"
              << "  -o, --output PATH       Assembly path; '-' writes stdout\n"
              << "  -O0 | -O1 | -O2        Optimization level (default: -O0)\n"
              << "  --color=MODE           auto, always, never (default: auto)\n"
              << "  --db PATH              Persist structured logs, metrics and traces in SQLite\n"
              << "  --traceparent VALUE    Continue a W3C trace across processes\n"
              << "  --summary              Print a human run summary to stderr\n"
              << "  -S                     Explicitly select assembly output\n"
              << "  -h, --help             Show this help\n"
              << "  --version              Show compiler version\n";
}

/** 中文：将错误同时投递给人工诊断和机器日志。英文：Deliver an error to both human diagnostics and machine logs. */
void report_errors(const std::vector<sysy::FrontendError>& errors,
                   sysy::DiagnosticEngine& diagnostics, sysy::Telemetry& telemetry,
                   std::string_view stage) {
    for (const auto& error : errors) {
        diagnostics.error(error.message, error.range);
        telemetry.log("error", error.message, telemetry.root_context(),
                      {{"stage", std::string(stage)},
                       {"line", std::to_string(error.range.begin.line)},
                       {"column", std::to_string(error.range.begin.column)}});
    }
}

/** 中文：测量阶段，同时产生可关联的 span 与 duration metric。英文：Time a stage and record a correlated span and duration metric. */
template <class Function, class Success>
auto timed(std::string name, sysy::Telemetry& telemetry, std::vector<StageTime>& stages,
           Function&& function, Success&& success) -> std::invoke_result_t<Function> {
    const auto started = Clock::now();
    auto span = telemetry.start_span(name, telemetry.root_context());
    auto result = [&] {
        try {
            return std::invoke(std::forward<Function>(function));
        } catch (...) {
            span.set_status("error");
            throw;
        }
    }();
    if (!std::invoke(std::forward<Success>(success), result)) span.set_status("error");
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    telemetry.metric("compiler.stage.duration", static_cast<double>(duration.count()), "ns",
                     span.context(), {{"stage", name}});
    stages.push_back({std::move(name), duration});
    return result;
}

/** 中文：将组装好的汇编写入文件并以原子重命名发布。英文：Publish a complete assembly file via a same-directory rename. */
std::optional<std::string> publish(std::string_view assembly, std::string_view destination) {
    if (destination == "-") {
        std::cout.write(assembly.data(), static_cast<std::streamsize>(assembly.size()));
        std::cout.flush();
        if (!std::cout) return "failed to write assembly to stdout";
        return std::nullopt;
    }
    const auto target = std::filesystem::path(destination);
    const auto nonce = std::to_string(Clock::now().time_since_epoch().count());
    auto temporary = target;
    temporary += ".tmp-" + nonce;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return "cannot create output file '" + temporary.string() + "'";
        file.write(assembly.data(), static_cast<std::streamsize>(assembly.size()));
        file.close();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return "failed while writing output file '" + temporary.string() + "'";
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return "cannot publish assembly at '" + target.string() + "': " + error.message();
    }
    return std::nullopt;
}

/** 中文：运行完整编译链；错误不会发布输出文件。英文：Run the complete pipeline without publishing output on error. */
int compile(const Options& options, sysy::Telemetry& telemetry,
            std::vector<StageTime>& stages, std::size_t& diagnostic_count) {
    std::string source_text;
    if (options.input == "-") {
        source_text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
        std::ifstream file(options.input, std::ios::binary);
        if (!file) {
            sysy::SourceFile source(options.input, "");
            sysy::DiagnosticEngine diagnostics(source, options.color);
            diagnostics.error("cannot read input file '" + options.input + "'");
            diagnostic_count = diagnostics.error_count();
            telemetry.log("error", "input file could not be read", telemetry.root_context(),
                          {{"path", options.input}});
            return 1;
        }
        source_text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (file.bad()) {
            sysy::SourceFile source(options.input, "");
            sysy::DiagnosticEngine diagnostics(source, options.color);
            diagnostics.error("failed while reading input file '" + options.input + "'");
            diagnostic_count = diagnostics.error_count();
            return 1;
        }
    }
    telemetry.metric("compiler.input.bytes", static_cast<double>(source_text.size()), "By");
    sysy::SourceFile source(options.input, std::move(source_text));
    sysy::DiagnosticEngine diagnostics(source, options.color);

    auto parsed = timed("parse", telemetry, stages, [&] { return sysy::parse(source.text()); },
                        [](const sysy::ParseResult& result) { return result.errors.empty(); });
    if (!parsed.errors.empty()) {
        report_errors(parsed.errors, diagnostics, telemetry, "parse");
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    auto semantic = timed("semantic", telemetry, stages,
                          [&] { return sysy::analyze(parsed.program); },
                          [](const sysy::SemanticResult& result) { return result.ok(); });
    if (!semantic.ok()) {
        report_errors(semantic.errors, diagnostics, telemetry, "semantic");
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    auto lowered = timed("lower", telemetry, stages,
                         [&] { return sysy::lower(parsed.program, semantic.model); },
                         [](const sysy::LowerResult& result) { return result.ok(); });
    if (!lowered.ok()) {
        report_errors(lowered.errors, diagnostics, telemetry, "lower");
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    std::vector<sysy::PassStats> pass_stats;
    const auto ir_errors = timed("optimize", telemetry, stages, [&] {
        return sysy::optimize(lowered.module, options.optimization,
                              telemetry.enabled() ? &pass_stats : nullptr);
    }, [](const std::vector<std::string>& errors) { return errors.empty(); });
    for (const auto& pass : pass_stats) {
        sysy::Attributes labels{{"function", pass.function}, {"pass", pass.pass}};
        telemetry.metric("compiler.pass.duration", static_cast<double>(pass.duration_ns), "ns",
                         telemetry.root_context(), labels);
        telemetry.metric("compiler.pass.instructions.before",
                         static_cast<double>(pass.instructions_before), "1",
                         telemetry.root_context(), labels);
        telemetry.metric("compiler.pass.instructions.after",
                         static_cast<double>(pass.instructions_after), "1",
                         telemetry.root_context(), labels);
        telemetry.metric("compiler.pass.blocks.before", static_cast<double>(pass.blocks_before),
                         "1", telemetry.root_context(), labels);
        telemetry.metric("compiler.pass.blocks.after", static_cast<double>(pass.blocks_after),
                         "1", telemetry.root_context(), labels);
    }
    if (!ir_errors.empty()) {
        for (const auto& error : ir_errors) {
            diagnostics.error("internal IR error: " + error);
            telemetry.log("error", error, telemetry.root_context(), {{"stage", "optimize"}});
        }
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    std::ostringstream assembly;
    auto emitted = timed("codegen", telemetry, stages,
                         [&] { return sysy::emit_rv64(lowered.module, assembly); },
                         [](const auto& result) { return result.has_value(); });
    if (!emitted) {
        diagnostics.error("target code generation failed: " + emitted.error());
        telemetry.log("error", emitted.error(), telemetry.root_context(), {{"stage", "codegen"}});
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    auto output = assembly.str();
    telemetry.metric("compiler.output.bytes", static_cast<double>(output.size()), "By");
    auto publication = timed("publish", telemetry, stages,
                             [&] { return publish(output, options.output); },
                             [](const auto& result) { return !result.has_value(); });
    if (publication) {
        diagnostics.error(*publication);
        telemetry.log("error", *publication, telemetry.root_context(), {{"stage", "publish"}});
        diagnostic_count = diagnostics.error_count();
        return 1;
    }
    diagnostic_count = diagnostics.error_count();
    return 0;
}

/** 中文：摘要按最耗时阶段排序展示，而非倾倒机器日志。英文：Summarize the slowest stages instead of dumping machine logs. */
void print_summary(const sysy::RunSummary& summary, const std::vector<StageTime>& stages,
                   std::size_t diagnostics, const Options& options) {
    const bool color = sysy::terminal_color_enabled(options.color, stderr);
    const auto bold = color ? "\033[1m" : "";
    const auto green = color ? "\033[32m" : "";
    const auto red = color ? "\033[31m" : "";
    const auto reset = color ? "\033[0m" : "";
    const auto ms = [](std::chrono::nanoseconds duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const auto elapsed = summary.duration + summary.database_finish_duration +
                         summary.database_close_duration;
    std::cerr << '\n' << bold << "Compilation summary" << reset << "  "
              << (summary.status == "ok" ? green : red) << summary.status << reset << '\n'
              << "  source       " << sysy::escape_terminal_text(options.input) << '\n'
              << "  output       " << sysy::escape_terminal_text(options.output) << '\n'
              << "  elapsed      " << std::fixed << std::setprecision(2) << ms(elapsed)
              << " ms\n  diagnostics  " << diagnostics << '\n';
    auto ordered = stages;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.duration > right.duration;
    });
    const auto visible = std::min<std::size_t>(ordered.size(), 4);
    for (std::size_t index = 0; index < visible; ++index) {
        std::cerr << "  " << std::left << std::setw(12) << ordered[index].name << std::right
                  << std::fixed << std::setprecision(2) << ms(ordered[index].duration) << " ms\n";
    }
    if (summary.database_requested) {
        std::cerr << "  SQLite       " << (summary.database_ok ? "saved" : "unavailable") << '\n';
        if (!summary.run_id.empty()) std::cerr << "    run ID     " << summary.run_id << '\n';
        std::cerr << "    setup      " << std::fixed << std::setprecision(2)
                  << ms(summary.database_setup_duration) << " ms\n"
                  << "    flush      " << ms(summary.database_finish_duration) << " ms\n"
                  << "    close      " << ms(summary.database_close_duration) << " ms\n";
    }
    if (!summary.database_error.empty())
        std::cerr << "  note         " << sysy::escape_terminal_text(summary.database_error) << '\n';
}

} // namespace

/** 中文：单体编译器入口；英文：Entry point for the monolithic compiler executable. */
int main(int argc, char* argv[]) {
    std::string error;
    auto options = parse_options(argc, argv, error);
    if (!options) {
        std::cerr << "compiler: error: " << sysy::escape_terminal_text(error)
                  << "\nTry 'compiler --help'.\n";
        return 2;
    }
    if (options->help) {
        print_help(options->color);
        return 0;
    }
    if (options->version) {
        std::cerr << "SysY compiler 0.1.0 (RV64GC / LP64D)\n";
        return 0;
    }
    sysy::Telemetry telemetry({.database_path = options->database,
                               .collect_summary = options->summary});
    telemetry.begin_run({.command = "compile",
                         .input_path = options->input,
                         .output_path = options->output,
                         .target = "riscv64-linux-gnu",
                         .optimization = "O" + std::to_string(options->optimization),
                         .compiler_version = "0.1.0",
                         .remote_parent = options->remote_parent});
    telemetry.event("compiler.compile.start", telemetry.root_context(),
                    {{"optimization", "O" + std::to_string(options->optimization)}});
    telemetry.log("info", "compilation started", telemetry.root_context(),
                  {{"input", options->input}, {"target", "riscv64-linux-gnu"}});
    std::vector<StageTime> stages;
    std::size_t diagnostics = 0;
    int status = 1;
    try {
        status = compile(*options, telemetry, stages, diagnostics);
    } catch (const std::exception& exception) {
        std::cerr << "compiler: error: internal failure: "
                  << sysy::escape_terminal_text(exception.what()) << '\n';
        telemetry.log("error", exception.what(), telemetry.root_context(), {{"stage", "driver"}});
    }
    telemetry.event("compiler.compile.finish", telemetry.root_context(),
                    {{"status", status == 0 ? "ok" : "error"},
                     {"diagnostics", std::to_string(diagnostics)}});
    telemetry.log(status == 0 ? "info" : "error", "compilation finished",
                  telemetry.root_context(),
                  {{"status", status == 0 ? "ok" : "error"},
                   {"diagnostics", std::to_string(diagnostics)}});
    auto summary = telemetry.finish_run(status == 0 ? "ok" : "error");
    if (!summary.database_error.empty() && !options->summary) {
        std::cerr << "compiler: warning: telemetry unavailable: "
                  << sysy::escape_terminal_text(summary.database_error) << '\n';
    }
    if (options->summary) print_summary(summary, stages, diagnostics, *options);
    return status;
}
