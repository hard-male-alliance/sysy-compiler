#include <compiler/telemetry.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

/// 即使 Release 构建定义 NDEBUG 也保留断言。 / Keep test checks active even in Release builds with
/// NDEBUG.
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                 \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

/// 查询单个整数，用于检查实际持久化而不依赖内部实现。 / Query one integer to verify real
/// persistence independently of implementation details.
int
scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    const int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

} // namespace

/// 覆盖禁用路径、失败隔离、关联上下文、并发写入和 SQL 持久化。 / Cover disabled mode, failure
/// isolation, correlated context, concurrent recording, and SQL persistence.
int
main() {
    const auto path = std::filesystem::path(".temp/telemetry_test.sqlite");
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);
    {
        sysy::Telemetry telemetry;
        CHECK(!telemetry.enabled());
        sysy::RunInfo info;
        info.command = "compile";
        telemetry.begin_run(info);
        CHECK(telemetry.start_span("disabled").context().trace_id.empty());
        const auto summary = telemetry.finish_run("ok");
        CHECK(!summary.database_requested);
        CHECK(summary.database_setup_duration.count() == 0);
        CHECK(summary.database_finish_duration.count() == 0);
        CHECK(summary.database_close_duration.count() == 0);
    }
    {
        sysy::TelemetryConfig config;
        config.collect_summary = true;
        sysy::Telemetry telemetry(config);
        CHECK(telemetry.enabled());
        sysy::RunInfo info;
        info.command = "compile";
        telemetry.begin_run(info);
        { auto span = telemetry.start_span("summary-only"); }
        telemetry.event("phase.done");
        const auto summary = telemetry.finish_run("ok");
        CHECK(summary.spans == 1 && summary.events == 1);
        CHECK(!summary.database_requested && summary.database_ok);
        CHECK(summary.database_setup_duration.count() == 0);
        CHECK(summary.database_finish_duration.count() == 0);
        CHECK(summary.database_close_duration.count() == 0);
    }
    {
        sysy::Telemetry telemetry(
            {.database_path = path.string(), .collect_summary = true, .batch_size = 2}
        );
        sysy::RunInfo info;
        info.command = "compile";
        info.input_path = "x.sy";
        info.attributes = {{"quoted", "a\"b"}};
        telemetry.begin_run(info);
        const auto root = telemetry.root_context();
        CHECK(root.trace_id.size() == 32 && root.span_id.size() == 16);
        {
            auto parent = telemetry.start_span("parse");
            auto child = telemetry.start_span("lex", parent.context(), {{"line", "1"}});
            telemetry.event("token", child.context());
            telemetry.metric("tokens", 1, "{token}", child.context());
            telemetry.log("info", "lexed", child.context());
        }
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&telemetry, root] {
                auto span = telemetry.start_span("worker", root);
                telemetry.event("task.done", span.context());
            });
        }
        for (auto& worker : workers)
            worker.join();
        const auto summary = telemetry.finish_run("ok");
        CHECK(summary.database_ok && summary.spans == 6 && summary.events == 5);
        CHECK(summary.metrics == 1 && summary.logs == 1);
        CHECK(summary.duration.count() >= 0);
        CHECK(summary.database_setup_duration.count() > 0);
        CHECK(summary.database_finish_duration.count() > 0);
        CHECK(summary.database_close_duration.count() > 0);
        CHECK(
            telemetry.finish_run("ignored").database_finish_duration
            == summary.database_finish_duration
        );
        CHECK(
            telemetry.finish_run("ignored").database_close_duration
            == summary.database_close_duration
        );
    }
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM run WHERE status='ok'") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM span") == 7); // Root plus six child spans.
    CHECK(scalar(db, "SELECT count(*) FROM event") == 5);
    CHECK(scalar(db, "SELECT count(*) FROM metric") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM log") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM run WHERE json_valid(attributes_json)") == 1);
    CHECK(
        scalar(
            db,
            "SELECT count(*) FROM span WHERE name='lex' AND parent_span_id IN (SELECT span_id FROM "
            "span WHERE name='parse')"
        )
        == 1
    );
    sqlite3_close(db);
    {
        // 未完成的运行由析构关闭连接，数据库保留 running 状态。 / Destruction closes an unfinished
        // run and leaves its database row marked running.
        sysy::TelemetryConfig config;
        config.database_path = path.string();
        sysy::Telemetry telemetry(config);
        sysy::RunInfo info;
        info.command = "unfinished";
        telemetry.begin_run(info);
    }
    CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM run WHERE status='running'") == 1);
    sqlite3_close(db);
    {
        const auto blocker = std::filesystem::path(".temp/telemetry_test_blocker");
        std::ofstream(blocker) << "not a directory";
        sysy::Telemetry telemetry(
            {.database_path = (blocker / "db.sqlite").string(), .collect_summary = true}
        );
        sysy::RunInfo info;
        info.command = "compile";
        telemetry.begin_run(info);
        { auto span = telemetry.start_span("compile"); }
        const auto summary = telemetry.finish_run("ok");
        CHECK(!summary.database_ok && !summary.database_error.empty());
        CHECK(summary.database_setup_duration.count() > 0);
        CHECK(summary.database_finish_duration.count() == 0);
        CHECK(summary.database_close_duration.count() > 0);
    }
}
