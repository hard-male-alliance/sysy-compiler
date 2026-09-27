/**
 * 中文：独立短进程 SQLite 生命周期探针；不链接或修改生产编译器。
 * English: Short-lived SQLite lifecycle probe; it neither links nor changes compiler code.
 *
 * Usage: sqlite_lifecycle_probe WAL|DELETE always|version PATH UNIQUE_ID
 */

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;

/** 中文：复刻生产 schema 的表、索引数量和 DDL 形状。 / Mirror production schema and index DDL. */
constexpr std::string_view schema = R"SQL(
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
)SQL";

/** 中文：准备与生产编译器同形状的六条 SQL。 / Prepare six production-shaped SQL statements. */
constexpr std::array<const char*, 6> statements = {
    "INSERT INTO "
    "run(run_id,trace_id,root_span_id,started_unix_ns,status,command,input_path,output_path,target,"
    "optimization,compiler_version,attributes_json) "
    "VALUES(?,'trace','root',0,'running','compile','','','','O0','probe','{}')",
    "UPDATE run SET duration_ns=?, status=? WHERE run_id=?",
    "INSERT INTO span VALUES(?,?,?,?,?,?,?,?,?,?)",
    "INSERT INTO event(run_id,trace_id,span_id,name,occurred_unix_ns,thread_id,attributes_json) "
    "VALUES(?,?,?,?,?,?,?)",
    "INSERT INTO "
    "metric(run_id,trace_id,span_id,name,value,unit,occurred_unix_ns,thread_id,attributes_json) "
    "VALUES(?,?,?,?,?,?,?,?,?)",
    "INSERT INTO "
    "log(run_id,trace_id,span_id,severity,message,occurred_unix_ns,thread_id,attributes_json) "
    "VALUES(?,?,?,?,?,?,?,?)"
};

/** 中文：执行固定 SQL 并保留 SQLite 错误。 / Execute fixed SQL and retain SQLite errors. */
bool
exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const auto code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (code == SQLITE_OK)
        return true;
    std::fprintf(stderr, "sqlite exec: %s\n", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    return false;
}

/** 中文：以现有 schema 版本作为快速路径判据。 / Check schema version for the fast path. */
int
user_version(sqlite3* db) {
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &query, nullptr) != SQLITE_OK)
        return -1;
    const int version = sqlite3_step(query) == SQLITE_ROW ? sqlite3_column_int(query, 0) : -1;
    sqlite3_finalize(query);
    return version;
}

/** 中文：将稳定标识绑定到文本参数。 / Bind a stable identifier to a text parameter. */
void
bind(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

/** 中文：将时间点差值转换为毫秒。 / Convert steady-clock intervals to milliseconds. */
double
ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
} // namespace

/** 中文：一次进程只完成一次连接、写入和关闭。 / Each process opens, writes, and closes once. */
int
main(int argc, char** argv) {
    if (argc != 5
        || (std::strcmp(argv[1], "WAL") && std::strcmp(argv[1], "DELETE"))
        || (std::strcmp(argv[2], "always") && std::strcmp(argv[2], "version"))) {
        std::fprintf(
            stderr,
            "usage: sqlite_lifecycle_probe WAL|DELETE always|version PATH UNIQUE_ID\n"
        );
        return 2;
    }
    const std::string id = argv[4];
    const auto started = Clock::now();
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(
            argv[3],
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr
        )
        != SQLITE_OK) {
        std::fprintf(stderr, "sqlite open: %s\n", db ? sqlite3_errmsg(db) : "unavailable");
        if (db)
            sqlite3_close(db);
        return 1;
    }
    sqlite3_busy_timeout(db, 250);
    const char* pragma = std::strcmp(argv[1], "WAL") == 0
                             ? "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"
                             : "PRAGMA journal_mode=DELETE; PRAGMA foreign_keys=ON;";
    bool ok = exec(db, pragma);
    if (ok && std::strcmp(argv[2], "always") == 0) {
        ok = exec(db, schema.data());
    } else if (ok) {
        const int version = user_version(db);
        ok = version == 1
             || (version == 0 && exec(db, schema.data()) && exec(db, "PRAGMA user_version=1"));
    }
    std::array<sqlite3_stmt*, 6> prepared{};
    for (std::size_t index = 0; ok && index < prepared.size(); ++index)
        ok = sqlite3_prepare_v2(db, statements[index], -1, &prepared[index], nullptr) == SQLITE_OK;
    if (ok) {
        bind(prepared[0], 1, id);
        ok = sqlite3_step(prepared[0]) == SQLITE_DONE;
    }
    const auto setup_end = Clock::now();
    if (ok)
        ok = exec(db, "BEGIN IMMEDIATE");
    if (ok) {
        auto* stmt = prepared[2];
        bind(stmt, 1, id);
        bind(stmt, 2, id);
        bind(stmt, 3, "root");
        sqlite3_bind_null(stmt, 4);
        bind(stmt, 5, "compiler.run");
        sqlite3_bind_int64(stmt, 6, 0);
        sqlite3_bind_int64(stmt, 7, 1000);
        bind(stmt, 8, "ok");
        bind(stmt, 9, "main");
        bind(stmt, 10, "{}");
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (ok)
        ok = exec(db, "COMMIT");
    if (ok) {
        auto* stmt = prepared[1];
        sqlite3_bind_int64(stmt, 1, 1000);
        bind(stmt, 2, "ok");
        bind(stmt, 3, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    const auto flush_end = Clock::now();
    for (auto* stmt : prepared)
        sqlite3_finalize(stmt);
    if (sqlite3_close(db) != SQLITE_OK)
        ok = false;
    const auto close_end = Clock::now();
    if (!ok) {
        std::fprintf(stderr, "sqlite lifecycle failed for %s\n", id.c_str());
        return 1;
    }
    std::printf(
        "{\"sqlite_version\":\"%s\",\"setup_ms\":%.6f,\"flush_ms\":%.6f,\"close_ms\":%.6f,\"inside_"
        "ms\":%.6f}\n",
        sqlite3_libversion(),
        ms(started, setup_end),
        ms(setup_end, flush_end),
        ms(flush_end, close_end),
        ms(started, close_end)
    );
    return 0;
}
