#include <compiler/telemetry.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <type_traits>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sysy {
namespace {

/// 将系统时钟编码为 Unix 纳秒，单调时钟单独用于耗时。 / Encode wall time as Unix nanoseconds; a
/// monotonic clock measures durations separately.
std::int64_t
unix_ns(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

/// 生成符合追踪约定的非零十六进制 ID。 / Generate a nonzero hexadecimal tracing ID.
std::string
random_hex(std::size_t bytes) {
    thread_local std::mt19937_64 random([] {
        std::random_device entropy;
        std::array<std::uint32_t, 8> seed_data{};
        for (auto& value : seed_data)
            value = entropy();
        std::seed_seq seed(seed_data.begin(), seed_data.end());
        return std::mt19937_64(seed);
    }());
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes * 2, '0');
    bool nonzero = false;
    for (std::size_t i = 0; i < bytes;) {
        auto sample = random();
        for (unsigned j = 0; j < 8 && i < bytes; ++j, ++i, sample >>= 8) {
            const auto byte = static_cast<unsigned>(sample & 0xffU);
            nonzero |= byte != 0;
            result[i * 2] = digits[byte >> 4];
            result[i * 2 + 1] = digits[byte & 15U];
        }
    }
    if (!nonzero)
        result.back() = '1';
    return result;
}

/// 检查外来上下文，避免把错误长度或字符写入关联字段。 / Validate imported context before using it
/// as a correlation key.
bool
valid_hex(std::string_view value, std::size_t length) {
    if (value.size() != length)
        return false;
    bool nonzero = false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero |= c != '0';
    }
    return nonzero;
}

/// 未知上下文自动关联本次运行根 span。 / Unknown contexts correlate to this invocation's root span.
std::string
correlated_span(const TraceContext& context, const TraceContext& root) {
    if (context.trace_id == root.trace_id && valid_hex(context.span_id, 16))
        return context.span_id;
    return root.span_id;
}

/// 转义一个 JSON 字符串；属性以文本处理，不信任调用方预先转义。 / Escape a JSON string;
/// caller-provided attributes are never trusted as pre-escaped JSON.
void
append_json_string(std::string& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15U];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

/// 属性按有序对象保存，重复键采用最后一个值。 / Store attributes as an ordered object; the final
/// duplicate key wins.
std::string
attributes_json(const Attributes& attributes) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : attributes) {
        if (!first)
            out += ',';
        first = false;
        append_json_string(out, key);
        out += ':';
        append_json_string(out, value);
    }
    out += '}';
    return out;
}

std::string
thread_id() {
    std::ostringstream out;
    out << std::this_thread::get_id();
    return out.str();
}

std::int64_t
process_id() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

void
bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

/// 复用预编译语句，同时保证失败的绑定不污染下一条记录。 / Reuse prepared statements and clear
/// bindings between records.
bool
step_done(sqlite3_stmt* stmt) {
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return ok;
}

struct SpanRecord {
    std::string span_id, parent_id, name, status, thread, attrs;
    std::int64_t start_ns, duration_ns;
};

struct EventRecord {
    std::string span_id, name, thread, attrs;
    std::int64_t time_ns;
};

struct MetricRecord {
    std::string span_id, name, unit, thread, attrs;
    double value;
    std::int64_t time_ns;
};

struct LogRecord {
    std::string span_id, severity, message, thread, attrs;
    std::int64_t time_ns;
};

using Record = std::variant<SpanRecord, EventRecord, MetricRecord, LogRecord>;

} // namespace

struct Telemetry::Impl {
    explicit Impl(TelemetryConfig value)
        : config(std::move(value)) {
        if (config.batch_size == 0)
            config.batch_size = 1;
    }

    ~Impl() {
        close_db();
    }

    /// 可重复调用的连接清理；未完成运行由析构兜底。 / Idempotent connection cleanup; the destructor
    /// handles unfinished runs.
    void
    close_db() {
        for (auto& stmt : statements) {
            if (stmt)
                sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        if (!db)
            return;
        if (sqlite3_close(db) == SQLITE_OK) {
            db = nullptr;
            return;
        }
        const std::string error = std::string("close: ") + sqlite3_errmsg(db);
        summary.database_ok = false;
        if (!summary.database_error.empty())
            summary.database_error += "; ";
        summary.database_error += error;
    }

    /// SQLite 只在配置非空路径且 begin_run 被调用后初始化。 / SQLite initializes only after
    /// begin_run with a nonempty configured path.
    bool
    open_db() {
        if (config.database_path.empty())
            return true;
        if (sqlite3_open_v2(
                config.database_path.c_str(),
                &db,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr
            )
            != SQLITE_OK)
            return fail("open");
        sqlite3_busy_timeout(db, std::max(0, config.busy_timeout_ms));
        if (!exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"))
            return false;
        if (!exec(R"SQL(
            CREATE TABLE IF NOT EXISTS run (
                run_id TEXT PRIMARY KEY, trace_id TEXT NOT NULL, root_span_id TEXT NOT NULL,
                remote_parent_span_id TEXT, started_unix_ns INTEGER NOT NULL,
                duration_ns INTEGER, status TEXT NOT NULL, command TEXT NOT NULL,
                input_path TEXT NOT NULL, output_path TEXT NOT NULL, target TEXT NOT NULL,
                optimization TEXT NOT NULL, compiler_version TEXT NOT NULL,
                process_id INTEGER, attributes_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS span (
                run_id TEXT NOT NULL REFERENCES run(run_id), trace_id TEXT NOT NULL,
                span_id TEXT NOT NULL, parent_span_id TEXT, name TEXT NOT NULL,
                started_unix_ns INTEGER NOT NULL, duration_ns INTEGER NOT NULL,
                status TEXT NOT NULL, thread_id TEXT NOT NULL, attributes_json TEXT NOT NULL,
                PRIMARY KEY(run_id, span_id)
            );
            CREATE TABLE IF NOT EXISTS event (
                id INTEGER PRIMARY KEY, run_id TEXT NOT NULL REFERENCES run(run_id),
                trace_id TEXT NOT NULL, span_id TEXT, name TEXT NOT NULL,
                occurred_unix_ns INTEGER NOT NULL, thread_id TEXT NOT NULL,
                attributes_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS metric (
                id INTEGER PRIMARY KEY, run_id TEXT NOT NULL REFERENCES run(run_id),
                trace_id TEXT NOT NULL, span_id TEXT, name TEXT NOT NULL,
                value REAL NOT NULL, unit TEXT NOT NULL, occurred_unix_ns INTEGER NOT NULL,
                thread_id TEXT NOT NULL, attributes_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS log (
                id INTEGER PRIMARY KEY, run_id TEXT NOT NULL REFERENCES run(run_id),
                trace_id TEXT NOT NULL, span_id TEXT, severity TEXT NOT NULL,
                message TEXT NOT NULL, occurred_unix_ns INTEGER NOT NULL,
                thread_id TEXT NOT NULL, attributes_json TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS span_trace_time ON span(trace_id, started_unix_ns);
            CREATE INDEX IF NOT EXISTS event_run_time ON event(run_id, occurred_unix_ns);
            CREATE INDEX IF NOT EXISTS metric_run_name ON metric(run_id, name);
            CREATE INDEX IF NOT EXISTS log_run_time ON log(run_id, occurred_unix_ns);
        )SQL"))
            return false;
        constexpr std::array<const char*, 6> sql = {
            "INSERT INTO run VALUES(?,?,?,?,?,NULL,'running',?,?,?,?,?,?,?,?)",
            "UPDATE run SET duration_ns=?, status=? WHERE run_id=?",
            "INSERT INTO span VALUES(?,?,?,?,?,?,?,?,?,?)",
            "INSERT INTO "
            "event(run_id,trace_id,span_id,name,occurred_unix_ns,thread_id,attributes_json) "
            "VALUES(?,?,?,?,?,?,?)",
            "INSERT INTO "
            "metric(run_id,trace_id,span_id,name,value,unit,occurred_unix_ns,thread_id,attributes_"
            "json) VALUES(?,?,?,?,?,?,?,?,?)",
            "INSERT INTO "
            "log(run_id,trace_id,span_id,severity,message,occurred_unix_ns,thread_id,attributes_"
            "json) VALUES(?,?,?,?,?,?,?,?)"
        };
        for (std::size_t i = 0; i < sql.size(); ++i) {
            if (sqlite3_prepare_v2(db, sql[i], -1, &statements[i], nullptr) != SQLITE_OK)
                return fail("prepare");
        }
        return true;
    }

    bool
    exec(const char* sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK)
            return true;
        return fail("SQL");
    }

    bool
    fail(std::string_view operation) {
        summary.database_ok = false;
        summary.database_error =
            std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : "SQLite unavailable");
        return false;
    }

    bool
    insert_run(const RunInfo& info) {
        auto* stmt = statements[0];
        bind_text(stmt, 1, summary.run_id);
        bind_text(stmt, 2, summary.trace_id);
        bind_text(stmt, 3, root.span_id);
        bind_text(stmt, 4, remote_parent_id);
        sqlite3_bind_int64(stmt, 5, unix_ns(started_wall));
        bind_text(stmt, 6, info.command);
        bind_text(stmt, 7, info.input_path);
        bind_text(stmt, 8, info.output_path);
        bind_text(stmt, 9, info.target);
        bind_text(stmt, 10, info.optimization);
        bind_text(stmt, 11, info.compiler_version);
        sqlite3_bind_int64(stmt, 12, process_id());
        bind_text(stmt, 13, attributes_json(info.attributes));
        return step_done(stmt) || fail("insert run");
    }

    bool
    insert(const Record& record) {
        return std::visit(
            [this](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, SpanRecord>) {
                    auto* stmt = statements[2];
                    bind_text(stmt, 1, summary.run_id);
                    bind_text(stmt, 2, summary.trace_id);
                    bind_text(stmt, 3, item.span_id);
                    bind_text(stmt, 4, item.parent_id);
                    bind_text(stmt, 5, item.name);
                    sqlite3_bind_int64(stmt, 6, item.start_ns);
                    sqlite3_bind_int64(stmt, 7, item.duration_ns);
                    bind_text(stmt, 8, item.status);
                    bind_text(stmt, 9, item.thread);
                    bind_text(stmt, 10, item.attrs);
                    return step_done(stmt) || fail("insert span");
                } else if constexpr (std::is_same_v<T, EventRecord>) {
                    auto* stmt = statements[3];
                    bind_text(stmt, 1, summary.run_id);
                    bind_text(stmt, 2, summary.trace_id);
                    bind_text(stmt, 3, item.span_id);
                    bind_text(stmt, 4, item.name);
                    sqlite3_bind_int64(stmt, 5, item.time_ns);
                    bind_text(stmt, 6, item.thread);
                    bind_text(stmt, 7, item.attrs);
                    return step_done(stmt) || fail("insert event");
                } else if constexpr (std::is_same_v<T, MetricRecord>) {
                    auto* stmt = statements[4];
                    bind_text(stmt, 1, summary.run_id);
                    bind_text(stmt, 2, summary.trace_id);
                    bind_text(stmt, 3, item.span_id);
                    bind_text(stmt, 4, item.name);
                    sqlite3_bind_double(stmt, 5, item.value);
                    bind_text(stmt, 6, item.unit);
                    sqlite3_bind_int64(stmt, 7, item.time_ns);
                    bind_text(stmt, 8, item.thread);
                    bind_text(stmt, 9, item.attrs);
                    return step_done(stmt) || fail("insert metric");
                } else {
                    auto* stmt = statements[5];
                    bind_text(stmt, 1, summary.run_id);
                    bind_text(stmt, 2, summary.trace_id);
                    bind_text(stmt, 3, item.span_id);
                    bind_text(stmt, 4, item.severity);
                    bind_text(stmt, 5, item.message);
                    sqlite3_bind_int64(stmt, 6, item.time_ns);
                    bind_text(stmt, 7, item.thread);
                    bind_text(stmt, 8, item.attrs);
                    return step_done(stmt) || fail("insert log");
                }
            },
            record
        );
    }

    /// 用单事务提交批次；失败后关闭进一步持久化但保留内存摘要。 / Commit a batch atomically; after
    /// failure, stop persistence but retain the in-memory summary.
    void
    flush() {
        if (queued.empty() || !summary.database_ok || !db)
            return;
        if (!exec("BEGIN IMMEDIATE"))
            return;
        for (const auto& record : queued) {
            if (!insert(record)) {
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
                queued.clear();
                return;
            }
        }
        if (!exec("COMMIT")) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            queued.clear();
            return;
        }
        queued.clear();
    }

    void
    enqueue(Record record) {
        if (!summary.database_requested || !summary.database_ok)
            return;
        queued.push_back(std::move(record));
        if (queued.size() >= config.batch_size)
            flush();
    }

    TelemetryConfig config;
    mutable std::mutex mutex;
    sqlite3* db = nullptr;
    std::array<sqlite3_stmt*, 6> statements{};
    std::vector<Record> queued;
    RunSummary summary;
    TraceContext root;
    std::string remote_parent_id;
    std::chrono::system_clock::time_point started_wall{};
    std::chrono::steady_clock::time_point started_steady{};
    bool started = false;
    bool finished = false;
};

Span::Span(
    Telemetry* owner,
    TraceContext context,
    std::string parent_id,
    std::string name,
    Attributes attributes
)
    : owner_(owner),
      context_(std::move(context)),
      parent_id_(std::move(parent_id)),
      name_(std::move(name)),
      attributes_(std::move(attributes)),
      started_wall_(std::chrono::system_clock::now()),
      started_steady_(std::chrono::steady_clock::now()) {}

Span::Span(Span&& other) noexcept {
    *this = std::move(other);
}

Span&
Span::operator=(Span&& other) noexcept {
    if (this == &other)
        return *this;
    finish();
    owner_ = std::exchange(other.owner_, nullptr);
    context_ = std::move(other.context_);
    parent_id_ = std::move(other.parent_id_);
    name_ = std::move(other.name_);
    status_ = std::move(other.status_);
    attributes_ = std::move(other.attributes_);
    started_wall_ = other.started_wall_;
    started_steady_ = other.started_steady_;
    return *this;
}

Span::~Span() {
    finish();
}

void
Span::finish() noexcept {
    if (auto* owner = std::exchange(owner_, nullptr))
        owner->end_span(*this);
}

Telemetry::Telemetry(TelemetryConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Telemetry::~Telemetry() = default;

bool
Telemetry::enabled() const noexcept {
    return !impl_->config.database_path.empty() || impl_->config.collect_summary;
}

void
Telemetry::begin_run(const RunInfo& info) {
    if (!enabled())
        return;
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (state.started)
        return;
    state.started = true;
    state.started_wall = std::chrono::system_clock::now();
    state.started_steady = std::chrono::steady_clock::now();
    state.summary.run_id = random_hex(16);
    const bool valid_parent =
        valid_hex(info.remote_parent.trace_id, 32) && valid_hex(info.remote_parent.span_id, 16);
    state.summary.trace_id = valid_parent ? info.remote_parent.trace_id : random_hex(16);
    state.remote_parent_id = valid_parent ? info.remote_parent.span_id : "";
    state.root = {state.summary.trace_id, random_hex(8)};
    state.summary.database_requested = !state.config.database_path.empty();
    if (state.summary.database_requested) {
        const auto db_start = std::chrono::steady_clock::now();
        if (state.open_db())
            state.insert_run(info);
        state.summary.database_setup_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - db_start
            );
    }
}

TraceContext
Telemetry::root_context() const {
    if (!enabled())
        return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->root;
}

Span
Telemetry::start_span(std::string name, TraceContext parent, Attributes attributes) {
    if (!enabled())
        return {};
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (!state.started || state.finished)
        return {};
    const std::string parent_id = correlated_span(parent, state.root);
    return Span(
        this,
        {state.summary.trace_id, random_hex(8)},
        parent_id,
        std::move(name),
        std::move(attributes)
    );
}

void
Telemetry::end_span(const Span& span) noexcept {
    try {
        auto& state = *impl_;
        std::lock_guard lock(state.mutex);
        if (!state.started || state.finished)
            return;
        ++state.summary.spans;
        if (!state.summary.database_requested || !state.summary.database_ok)
            return;
        state.enqueue(
            SpanRecord{
                span.context_.span_id,
                span.parent_id_,
                span.name_,
                span.status_.empty() ? "ok" : span.status_,
                thread_id(),
                attributes_json(span.attributes_),
                unix_ns(span.started_wall_),
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - span.started_steady_
                )
                    .count()
            }
        );
    } catch (
        ...
    ) { /* 观测错误不得改变编译结果。 / Observability must not change compiler behavior. */
    }
}

void
Telemetry::event(std::string name, TraceContext context, Attributes attributes) {
    if (!enabled())
        return;
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (!state.started || state.finished)
        return;
    ++state.summary.events;
    if (!state.summary.database_requested || !state.summary.database_ok)
        return;
    state.enqueue(
        EventRecord{
            correlated_span(context, state.root),
            std::move(name),
            thread_id(),
            attributes_json(attributes),
            unix_ns(std::chrono::system_clock::now())
        }
    );
}

void
Telemetry::metric(
    std::string name,
    double value,
    std::string unit,
    TraceContext context,
    Attributes attributes
) {
    if (!enabled() || !std::isfinite(value))
        return;
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (!state.started || state.finished)
        return;
    ++state.summary.metrics;
    if (!state.summary.database_requested || !state.summary.database_ok)
        return;
    state.enqueue(
        MetricRecord{
            correlated_span(context, state.root),
            std::move(name),
            std::move(unit),
            thread_id(),
            attributes_json(attributes),
            value,
            unix_ns(std::chrono::system_clock::now())
        }
    );
}

void
Telemetry::log(
    std::string severity,
    std::string message,
    TraceContext context,
    Attributes attributes
) {
    if (!enabled())
        return;
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (!state.started || state.finished)
        return;
    ++state.summary.logs;
    if (!state.summary.database_requested || !state.summary.database_ok)
        return;
    state.enqueue(
        LogRecord{
            correlated_span(context, state.root),
            std::move(severity),
            std::move(message),
            thread_id(),
            attributes_json(attributes),
            unix_ns(std::chrono::system_clock::now())
        }
    );
}

RunSummary
Telemetry::finish_run(std::string status) {
    auto& state = *impl_;
    if (!enabled())
        return {};
    std::lock_guard lock(state.mutex);
    if (!state.started || state.finished)
        return state.summary;
    state.finished = true;
    state.summary.status = std::move(status);
    state.summary.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - state.started_steady
    );
    if (state.summary.database_requested && state.summary.database_ok) {
        const auto db_start = std::chrono::steady_clock::now();
        state.enqueue(
            SpanRecord{
                state.root.span_id,
                state.remote_parent_id,
                "compiler.run",
                state.summary.status,
                thread_id(),
                "{}",
                unix_ns(state.started_wall),
                state.summary.duration.count()
            }
        );
        state.flush();
        if (state.summary.database_ok) {
            auto* stmt = state.statements[1];
            sqlite3_bind_int64(stmt, 1, state.summary.duration.count());
            bind_text(stmt, 2, state.summary.status);
            bind_text(stmt, 3, state.summary.run_id);
            if (!step_done(stmt))
                state.fail("finish run");
        }
        state.summary.database_finish_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - db_start
            );
    }
    if (state.db) {
        const auto db_start = std::chrono::steady_clock::now();
        state.close_db();
        state.summary.database_close_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - db_start
            );
    }
    return state.summary;
}

} // namespace sysy
