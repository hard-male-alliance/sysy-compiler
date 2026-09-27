# Compiler command-line interface

The driver accepts exactly one source file per invocation. It emits RISC-V RV64GC / LP64D GNU/Linux assembly, not an object file or linked executable.

```text
compiler [options] input.sy [-o output.s]
compiler [options] - -o -
```

| Option | Behavior |
| --- | --- |
| `-o PATH`, `--output PATH` | Assembly destination. Omit to replace the input extension with `.s`; `-` means standard output. The driver rejects an output path that aliases the input. |
| `-O0`, `-O1`, `-O2` | Optimization level; default `-O0`. `-O0` validates IR but does not run optional transforms. `-O1` adds unreachable-block removal, eligible scalar mem2reg, sparse conditional constant propagation, and dead-code elimination. `-O2` also adds dominance-scoped common-subexpression elimination. These are the present implementation, not a general optimization guarantee. |
| `-S` | Explicit assembly selection; currently redundant because assembly is the only output format. |
| `--color=auto\|always\|never` | Human-facing stderr styling. A separate value after `--color` is accepted. `auto` uses terminal capabilities and honors `NO_COLOR`, `CLICOLOR_FORCE`, `CLICOLOR=0`, and `TERM=dumb`. `always` forces ANSI styling, including into redirected output. |
| `--db PATH`, `--db=PATH` | Persist structured run, span, event, metric, and log records to a local SQLite database. No database is opened when absent. The database path must not alias the input or output, and neither may alias its `-wal`, `-shm`, or `-journal` sidecars. |
| `--summary` | Print a human-readable per-run status, elapsed time, error count, and up to four slowest stages to stderr. With `--db`, also show database status plus SQLite setup, flush, and close timings. Works without `--db`; this summary is not a full trace. |
| `--traceparent VALUE` | Continue a trace using a valid W3C version-`00` `traceparent` value, for example `00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`. Trace and parent span IDs must be nonzero. The present implementation imports IDs for correlation; it is **not** an OTLP exporter or a distributed scheduler. |
| `-h`, `--help`, `--version` | Print human-facing information to stderr. |
| `--` | Treat subsequent arguments as positional input, useful for a filename beginning with `-`. |

The driver accepts `-` for standard input and, if no output is provided, defaults its output to standard output. An explicit `-o -` is still clearest in scripts. The CLI does not accept `--output=PATH` or `--traceparent=VALUE` in the current implementation.

## Streams, failures, and output publication

Diagnostics include `path:line:column`, severity, a bounded source excerpt, and a caret/underline when a source range is available. They go only to **stderr**, as do help, version, telemetry warnings, and the optional summary. Standard output is reserved for assembly with `-o -`. A source error does not publish an output file: an already existing destination remains untouched. File output is first written to a same-directory temporary file and then renamed to the destination, replacing a prior output on successful publication. If the filesystem or platform refuses the rename, publication fails rather than deleting the prior output. Input, output, SQLite database, and SQLite sidecar paths are checked for lexical, canonical, and existing-file aliases before compilation; this is a path-safety guard, not a guarantee against concurrent filesystem changes.

Exit status is `0` for success/help/version, `1` for compilation or output failure, and `2` for command-line usage errors. Telemetry database failures do **not** change an otherwise successful compile status. With `--summary`, the database failure appears in the summary; without it, a warning appears on stderr. The database is not an audit log: a crash may leave a `running` row or lose a final uncommitted batch.

## SQLite observability

`--db .temp/compile.sqlite --summary` enables both machine records and a concise stderr readout. The database has `run`, `span`, `event`, `metric`, and `log` tables; run and stage records carry trace/span IDs so phase timings and errors can be correlated. Input/output byte counts and stage-duration metrics are recorded. When telemetry is enabled, each optimization pass additionally records its duration and before/after instruction and basic-block counts, labeled by function and pass. The human summary reports only the slowest pipeline stages, not every pass; its SQLite `setup`, `flush`, and `close` entries expose database overhead separately from the compiler-stage timings. Database paths and diagnostic messages may be sensitive; use a suitable local path and retention policy. SQLite WAL mode permits concurrent readers but is not appropriate for a network filesystem. See the [schema and lifecycle note](architecture/telemetry.md) and [SQLite WAL documentation](https://www.sqlite.org/wal.html).

The trace context follows the parent-ID model of the [W3C Trace Context recommendation](https://www.w3.org/TR/trace-context/). Passing `--traceparent` makes the invocation a child of the supplied span; it does not automatically propagate context to a generated program or to future compiler worker processes.
