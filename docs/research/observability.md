# Compiler diagnostics and observability design research

Scope: SysY single executable, C++23, cross-platform CLI, optional parallel task graph. Decision purpose: preserve a clean human CLI while obtaining queryable per-run logs, metrics, and causal traces in SQLite without regressing short compilations. This is a design recommendation, not a measured implementation result. Reviewed 2026-09-27.

## Evidence and design implications

| Evidence | What it supports | Boundary |
| --- | --- | --- |
| [Clang Users Manual, diagnostics](https://clang.llvm.org/docs/UsersManual.html) | Clang's human diagnostic model combines location, severity, source excerpt, caret/range, notes, and confident fix-its. Color defaults to capable terminals; `NO_COLOR` disables unless explicitly overridden. | A model for presentation, not a SysY diagnostic specification. |
| [GCC diagnostic formatting](https://gcc.gnu.org/onlinedocs/gcc/Diagnostic-Message-Formatting-Options.html) | `auto` color tests **stderr** as a terminal; explicit always/never modes; machine-readable diagnostics are separate from human text. | GCC's configurable default differs by build, so copy the explicit policy, not its accidental default. |
| [Windows GetConsoleMode](https://learn.microsoft.com/windows/console/getconsolemode) | A Windows console handle can be tested, then `ENABLE_VIRTUAL_TERMINAL_PROCESSING` enabled for ANSI color. | A redirected pipe is not a console handle; never inject ANSI there in auto mode. |
| [Clang time trace](https://clang.llvm.org/docs/UsersManual.html#options-to-emit-resource-consumption-reports) | Compiler-stage time tracing is production-proven; Clang's default minimum event duration is 500 microseconds and verbose traces can be 2–3x larger. | Chrome JSON export is optional; SQLite is the primary local store here. |
| [OpenTelemetry trace API](https://opentelemetry.io/docs/specs/otel/trace/api/), [W3C Trace Context](https://www.w3.org/TR/trace-context/) | Immutable context has 16-byte trace ID, 8-byte span ID, parent, flags; spans have status/events/links; explicit parent context supports async tasks. W3C `traceparent` is interoperable across process boundaries. | A custom SQLite backend need not claim full OTLP compliance. |
| [OpenTelemetry log data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/) | Structured logs carry timestamp, severity, body, attributes, trace/span IDs; span ID implies trace ID. | Keep user-facing diagnostics separate from internal logs. |
| [SQLite WAL](https://www.sqlite.org/wal.html), [transactions](https://www.sqlite.org/lang_transaction.html) | WAL permits concurrent readers but only one writer; batch writes in transactions. `NORMAL` avoids per-commit sync but permits loss on power failure. WAL is not suitable on network filesystems. | Important 2026 safety caveat: [SQLite's WAL-reset bug](https://www.sqlite.org/wal.html#the_wal_reset_bug) affects rare concurrent-writer/checkpointer races in versions through 3.51.2; use 3.51.3+ or patched 3.44.6/3.50.7 when embedding SQLite. |
| [Huemer et al., ECOOP 2024](https://doi.org/10.4230/LIPIcs.ECOOP.2024.20) | Outlier-driven, fine-grained compilation-time analysis exposed regressions missed by whole-run metrics in GraalVM; reported two optimizations improving compilation time 2.25–9.45%. | GraalVM workloads and JIT architecture differ; use as a hypothesis for SysY benchmark design, not an expected speedup. |
| [Zhang et al., NSDI 2023, Hindsight](https://www.usenix.org/conference/nsdi23/presentation/zhang-lei) | Retroactive capture can retain rare error/long-tail traces without persisting all normal traces. | Their distributed, high-throughput deployment results do not transfer directly to a short-lived compiler CLI; a bounded in-memory ring is an optional later experiment. |
| [Dornbusch & Vahrenhold, ICER 2024](https://doi.org/10.1145/3632620.3671105) | 12 qualitative interviews associate compiler-error interaction with understanding and sense of belonging. | Small, qualitative educational study; does not prove any specific rendering improves task completion. User testing remains necessary. |

## Recommended CLI contract

- `stdout` is reserved for requested compiler artifact only (for example textual IR/assembly to `-`); human help may use stdout only for an explicit `--help` invocation with no artifact. All errors, warnings, progress (if any), and optional per-run summary go to `stderr`. Machine telemetry/log/trace records go to the database, not mixed into streams.
- `--color=auto|always|never` with `auto` default. An explicit option wins; otherwise honor non-empty `NO_COLOR`, then check **stderr** TTY capability. On Windows enable VT processing for console stderr, or choose plain output if unavailable. `always` should be documented to emit ANSI even to pipes. Do not use `TERM` alone as proof of capability.
- Diagnostic as typed data: stable code, severity, primary source span (file, byte offsets and human 1-based line/column), message, zero or more labeled secondary spans, notes, and an optional machine-applicable fix. The renderer alone owns ANSI escapes and stream selection. Handle tabs, UTF-8 width, CRLF, truncated long lines, EOF, missing source file, and multiple diagnostics deterministically. A source excerpt must never be treated as terminal control text; escape C0/C1 controls. Avoid claiming a fix is safe when ambiguous.
- Summary (`--summary`, off by default) should be a short stderr block: status, elapsed wall time, input/output, warning/error counts, top 3 longest stages, telemetry database/run ID if active; clearly label estimates or incomplete data. Do not turn a successful compile into a failure solely because observability export failed, unless user explicitly selects a strict telemetry mode.

## Proposed event/data model (schema v1)

IDs are `BLOB` (16-byte trace, 8-byte span) rather than high-cardinality text; render lowercase hex at CLI/exports. UTC timestamps enable cross-process correlation; per-process steady-clock durations avoid wall-clock jumps. `run_id` is unique across invocations; `trace_id` may encompass child processes. Schema constraints and foreign keys should be enabled and migrations keyed by `PRAGMA user_version`.

```sql
CREATE TABLE runs (
  run_id BLOB PRIMARY KEY CHECK(length(run_id)=16),
  trace_id BLOB NOT NULL CHECK(length(trace_id)=16),
  started_utc_ns INTEGER NOT NULL,
  ended_utc_ns INTEGER,
  duration_ns INTEGER,
  compiler_version TEXT NOT NULL,
  target TEXT,
  command TEXT NOT NULL,          -- sanitized invocation, not raw secrets
  input_path TEXT,
  output_path TEXT,
  status TEXT NOT NULL CHECK(status IN ('running','ok','compile_error','io_error','internal_error','interrupted')),
  exit_code INTEGER,
  error_count INTEGER NOT NULL DEFAULT 0,
  warning_count INTEGER NOT NULL DEFAULT 0,
  dropped_events INTEGER NOT NULL DEFAULT 0,
  telemetry_error TEXT
);
CREATE TABLE spans (
  run_id BLOB NOT NULL REFERENCES runs(run_id),
  trace_id BLOB NOT NULL CHECK(length(trace_id)=16),
  span_id BLOB NOT NULL CHECK(length(span_id)=8),
  parent_span_id BLOB,
  name TEXT NOT NULL,             -- bounded vocabulary: lex, parse, sema, opt.pass, emit, link
  kind TEXT NOT NULL,
  process_id INTEGER,
  thread_id TEXT,
  start_utc_ns INTEGER NOT NULL,
  end_utc_ns INTEGER NOT NULL,
  duration_ns INTEGER NOT NULL CHECK(duration_ns>=0),
  status TEXT NOT NULL,
  attrs_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY (trace_id, span_id)
);
CREATE INDEX spans_run_time ON spans(run_id, start_utc_ns);
CREATE TABLE span_links (
  trace_id BLOB NOT NULL,
  span_id BLOB NOT NULL,
  linked_trace_id BLOB NOT NULL,
  linked_span_id BLOB NOT NULL,
  relation TEXT NOT NULL,        -- e.g. task_dependency
  FOREIGN KEY(trace_id,span_id) REFERENCES spans(trace_id,span_id)
);
CREATE TABLE logs (
  run_id BLOB NOT NULL REFERENCES runs(run_id),
  seq INTEGER NOT NULL,
  timestamp_utc_ns INTEGER NOT NULL,
  severity INTEGER NOT NULL,     -- OTel-aligned integer ranges
  event_name TEXT NOT NULL,      -- stable machine key
  body TEXT NOT NULL,
  trace_id BLOB,
  span_id BLOB,
  attrs_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY (run_id,seq)
);
CREATE TABLE metrics (
  run_id BLOB NOT NULL REFERENCES runs(run_id),
  name TEXT NOT NULL,            -- stable key, e.g. tokens_total, peak_rss_bytes
  unit TEXT NOT NULL,
  scope TEXT NOT NULL,           -- run, stage, pass
  scope_id TEXT NOT NULL DEFAULT '',
  value REAL NOT NULL,
  PRIMARY KEY (run_id,name,scope,scope_id)
);
CREATE TABLE diagnostics (
  run_id BLOB NOT NULL REFERENCES runs(run_id),
  seq INTEGER NOT NULL,
  code TEXT NOT NULL,
  severity TEXT NOT NULL,
  file TEXT,
  start_byte INTEGER,
  end_byte INTEGER,
  message TEXT NOT NULL,
  attrs_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY (run_id,seq)
);
```

Store sparse/explanatory attributes in JSON initially, but keep common filter/join/group-by dimensions in typed columns. Do not store full source, argv secrets, environment, or raw binary artifact without explicit consent. If diagnostics are persisted, their rendered ANSI text is not persisted: store semantic fields.

## Low-overhead execution path and causal model

1. Parse CLI and decide observability mode before opening SQLite. Disabled mode uses a no-op sink and does not allocate strings/JSON, IDs, or database objects on hot paths. Measure this assertion with a compile-time benchmark.
2. When enabled, create `run_id` and root context, then record coarse phases by default. Maintain counter increments and span timestamps with a steady clock. Gate fine-grained spans by duration threshold or trace-detail flag; never log each token by default. Use RAII span guards to close on normal/error/exception paths.
3. Pass an immutable `TraceContext` explicitly with each task submitted to a future parallel executor. One task gets one span (or a stable operation group); task dependencies become links when no unique parent exists. Never infer causality from thread ID or overlapping timestamps. For subprocesses, pass W3C-compatible `traceparent` only to trusted child processes and validate any inbound context; child clock skew makes cross-process wall-time subtraction suspect, so use parent/child and links for causality and local monotonic durations for measured work.
4. Worker threads enqueue bounded, compact records; one per-process writer connection batches prepared inserts in a transaction at end of compile or bounded intervals for long runs. For a very short compile, avoid DB open/fsync on the critical path when mode is off; when on, measure open/schema/commit separately and report telemetry overhead. If many processes share one DB, set busy timeout and WAL only on local filesystem, retry bounded `SQLITE_BUSY`, and consider one DB per worker plus later merge if write contention dominates.
5. On buffer overflow, count dropped events and preserve run status/errors before debug detail. A writer failure degrades telemetry but not compilation semantics by default; emit at most one human stderr warning and record an incomplete summary if possible. Ensure a crash/interruption leaves `running` rows interpretable as incomplete; atomic final batch is preferable when feasible.

## Validation and falsification plan

- Golden CLI tests: stderr/stdout byte separation, `NO_COLOR`, each `--color` mode, redirected stderr, Windows console fallback, UTF-8/tab/control-character source excerpt, multiple notes/ranges, missing file and syntax/semantic errors. Redirection must contain no ANSI in auto mode.
- Database tests: schema migration, foreign keys, interruption/exception, locked DB, concurrent invocations, WAL checkpoint/readers, full disk, malformed traceparent, oversized attributes and path privacy. Validate run/span/log correlations and monotonic nonnegative durations.
- Performance A/B: empty/small/large SysY programs, warm/cold start, telemetry off/coarse/fine, median and p95, process spawn-to-exit vs phase timings, CPU and peak RSS. Performance claims require CI or controlled local numbers; do not assume SQLite WAL or buffering is free. Investigate phase/function/pass outliers (ECOOP 2024 insight) rather than optimize only average whole-run time.
- Revisit architecture if telemetry-disabled startup rises materially, enabled p95 is unacceptably high, dropped events occur in normal workloads, SQLite lock wait dominates, or user studies show diagnostics do not lead users to the next corrective action.
