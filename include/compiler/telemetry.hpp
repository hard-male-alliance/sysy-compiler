#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sysy {

/// 结构化属性；键和值均为 UTF-8 字符串，存储时转义为 JSON。 / Structured UTF-8 attributes serialized as escaped JSON.
using Attributes = std::vector<std::pair<std::string, std::string>>;

/// 可跨线程或进程传递的追踪上下文；空 span_id 表示无父节点。 / Trace context transferable across threads or processes; an empty span_id means no parent.
struct TraceContext {
    std::string trace_id;
    std::string span_id;
};

/// 遥测配置；空数据库路径意味着不打开 SQLite。 / Telemetry configuration; an empty database path never opens SQLite.
struct TelemetryConfig {
    std::string database_path;
    bool collect_summary = false;
    std::size_t batch_size = 128;
    int busy_timeout_ms = 250;
};

/// 一次编译的静态元数据。 / Static metadata for one compiler invocation.
struct RunInfo {
    std::string command;
    std::string input_path;
    std::string output_path;
    std::string target;
    std::string optimization;
    std::string compiler_version;
    Attributes attributes;
    TraceContext remote_parent;
};

/// 人类摘要的数据模型；调用者决定如何着色和输出。 / Data model for a human summary; the caller owns formatting and output.
struct RunSummary {
    std::string run_id;
    std::string trace_id;
    std::string status;
    std::chrono::nanoseconds duration{};
    /// begin_run 的打开库、设置 schema 和插入 run 耗时；未请求数据库时为零。 / Time spent opening/configuring SQLite and inserting the run in begin_run; zero without a database.
    std::chrono::nanoseconds database_setup_duration{};
    /// finish_run 的根 span 入队、最终批次提交与 run 更新耗时；未请求数据库时为零。 / Time spent queueing the root span, committing the final batch, and updating the run in finish_run; zero without a database.
    std::chrono::nanoseconds database_finish_duration{};
    /// finish_run 的语句释放与 SQLite 连接关闭耗时；未打开句柄时为零。 / Time spent finalizing statements and closing SQLite in finish_run; zero when no handle was opened.
    std::chrono::nanoseconds database_close_duration{};
    std::uint64_t spans = 0;
    std::uint64_t events = 0;
    std::uint64_t metrics = 0;
    std::uint64_t logs = 0;
    bool database_requested = false;
    bool database_ok = true;
    std::string database_error;
};

class Telemetry;

/// 自动结束的 span；生命周期必须短于所属 Telemetry。 / RAII span; it must not outlive its owning Telemetry.
class Span {
public:
    Span() noexcept = default;
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;
    ~Span();

    /// 返回可显式传给子任务的上下文。 / Return context that can be explicitly passed to child tasks.
    [[nodiscard]] const TraceContext& context() const noexcept { return context_; }

    /// 设置结果，析构时写入；空值表示成功。 / Set the result written at destruction; empty means success.
    void set_status(std::string status) { status_ = std::move(status); }

private:
    friend class Telemetry;
    Span(Telemetry* owner, TraceContext context, std::string parent_id,
         std::string name, Attributes attributes);
    void finish() noexcept;

    Telemetry* owner_ = nullptr;
    TraceContext context_;
    std::string parent_id_;
    std::string name_;
    std::string status_;
    Attributes attributes_;
    std::chrono::system_clock::time_point started_wall_{};
    std::chrono::steady_clock::time_point started_steady_{};
};

/// 单次编译的观测记录器。 / Observability recorder for a single compiler invocation.
///
/// 用法 / Usage:
/// @code
/// sysy::Telemetry telemetry({.database_path = "runs.sqlite", .collect_summary = true});
/// telemetry.begin_run({.command = "compile", .input_path = "hello.sy"});
/// { auto parse = telemetry.start_span("parse"); /* parser work */ }
/// auto summary = telemetry.finish_run("ok");
/// @endcode
/// 数据库故障不抛出且不会改变编译结果；查询 summary.database_error 进行提示。
/// Database failures do not throw or affect compilation; inspect summary.database_error.
class Telemetry {
public:
    explicit Telemetry(TelemetryConfig config = {});
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;
    ~Telemetry();

    /// 开始一次运行；必须先于记录调用，且每实例仅调用一次。 / Start one run, once per instance, before recording.
    void begin_run(const RunInfo& info);

    /// 创建 span；parent 为空时挂在本次运行的根节点下。 / Create a span; an empty parent attaches to the run root.
    [[nodiscard]] Span start_span(std::string name, TraceContext parent = {}, Attributes attributes = {});

    /// 记录离散事件。 / Record a discrete event.
    void event(std::string name, TraceContext context = {}, Attributes attributes = {});

    /// 记录数值指标，单位采用可读的 UCUM 风格文本。 / Record a numeric metric with a human-readable UCUM-style unit.
    void metric(std::string name, double value, std::string unit = {},
                TraceContext context = {}, Attributes attributes = {});

    /// 记录机器可读日志；诊断仍由 CLI 单独写入 stderr。 / Record a machine-readable log; CLI diagnostics still go to stderr separately.
    void log(std::string severity, std::string message, TraceContext context = {},
             Attributes attributes = {});

    /// 刷新并结束运行；重复调用返回相同摘要。 / Flush and finish; subsequent calls return the same summary.
    [[nodiscard]] RunSummary finish_run(std::string status);

    /// 是否收集任何信号；关闭时所有记录调用快速返回。 / Whether any signal is collected; disabled calls return immediately.
    [[nodiscard]] bool enabled() const noexcept;

    /// 返回根上下文，供任务图显式传播。 / Return root context for explicit task-graph propagation.
    [[nodiscard]] TraceContext root_context() const;

private:
    friend class Span;
    void end_span(const Span& span) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sysy
