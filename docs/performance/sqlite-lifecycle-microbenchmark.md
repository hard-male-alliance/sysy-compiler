# SQLite short-process lifecycle experiment (2026-09-27)

## Decision question

Optional telemetry added roughly 20–24 ms to fresh-process SysY compilation on the local Windows host. Separate compiler summary timings attributed much of that to SQLite setup, flush and close, but did not identify one overwhelming step. This microbenchmark tests **two narrow implementation alternatives** before changing production code:

1. Keep the existing `WAL` write-ahead journal or switch to `DELETE` rollback journal?
2. Execute idempotent schema DDL on every open (existing behavior), or read `PRAGMA user_version` and skip DDL for a known version?

The decision purpose is **not** to minimize one isolated setup span. A journal change also shifts costs into commit/close and changes concurrency/operational behavior; judge the entire short-lived process and preserve durability semantics.

## Reproduction and controls

Source: `benchmarks/sqlite_lifecycle_probe.cpp`; driver: `benchmarks/sqlite_lifecycle_runner.py`. The probe is an independent MinGW C++23 executable linked against the same system SQLite 3.53.1 used by the compiler. Each invocation opens one connection with `SQLITE_OPEN_FULLMUTEX`, sets busy timeout and foreign keys, applies the selected journal/schema policy, prepares six production-shaped statements, inserts a run, performs one `BEGIN IMMEDIATE`/root-span/`COMMIT`, updates the run, finalizes statements, and closes the connection. Its five-table/four-index schema mirrors the compiler's current DDL. This is **not** a full replay of every compiler metric/log record or tracing allocation, so absolute times must not be extrapolated to the compiler.

One process is launched per sample. Four persistent, separate databases prevent journal switching between samples; five warm-ups create and prime each schema so the measured question is repeated **steady-state opening**, not first-install migration. The four variants run in a seeded randomized order each round. There are 41 measured rounds per replicate and two independent replicates. The runner records internal setup/flush/close and external subprocess time, with per-round differences and seeded bootstrap intervals for the median. Its stdout JSON capture is identical for all modes. All files live in `.cache/performance` or `.temp/performance`; no global cache flushing or OS tuning was done.

```powershell
New-Item -ItemType Directory -Force .cache/performance | Out-Null
g++ -std=c++23 -O2 -Wall -Wextra -Wpedantic benchmarks/sqlite_lifecycle_probe.cpp -lsqlite3 -o .cache/performance/sqlite_lifecycle_probe.exe
python benchmarks/sqlite_lifecycle_runner.py --probe .cache/performance/sqlite_lifecycle_probe.exe --repetitions 41 --warmups 5 --out .temp/performance/sqlite_micro_1
python benchmarks/sqlite_lifecycle_runner.py --probe .cache/performance/sqlite_lifecycle_probe.exe --repetitions 41 --warmups 5 --out .temp/performance/sqlite_micro_2
```

Windows 11 build 26200, Intel Family 6 Model 154 Stepping 3, MinGW GCC 16.1.0, SQLite 3.53.1. Probe SHA-256 begins `389dea7f5f746e19`; full hash and raw data are in the two `results.json` files. Each DB was checked after measurement: 46 `run` and 46 `span` rows (5 warm-up + 41 measured), journal mode matched the requested variant, and `user_version` was 0 for always-DDL versus 1 for version-fast-path.

## Results

Median milliseconds, replicate 1 / replicate 2:

| Variant | Setup | Flush/update | Close | External process |
| --- | ---: | ---: | ---: | ---: |
| WAL + always DDL | 6.19 / 6.59 | 2.89 / 2.87 | 2.77 / 2.77 | 26.33 / 26.90 |
| WAL + version fast path | 6.31 / 6.59 | 3.09 / 2.93 | 2.80 / 2.79 | 27.10 / 26.37 |
| DELETE + always DDL | 7.95 / 7.29 | 12.99 / 12.47 | 0.08 / 0.07 | 33.62 / 33.59 |
| DELETE + version fast path | 7.10 / 6.75 | 12.98 / 11.35 | 0.07 / 0.08 | 34.59 / 35.08 |

More decision-relevant **paired median differences** (left minus right, 95% bootstrap interval):

| Comparison | Replicate 1 external | Replicate 2 external | Interpretation |
| --- | ---: | ---: | --- |
| WAL always DDL − WAL version | −0.19 ms [−1.14, +1.53] | +0.15 ms [−0.15, +0.95] | No detectable end-to-end gain from the version fast path in WAL mode. |
| DELETE always DDL − DELETE version | +1.60 ms [+0.65, +2.69] | +1.44 ms [+0.55, +3.10] | Small gain within DELETE mode, but this mode is slower overall. |
| WAL always DDL − DELETE always DDL | −7.53 ms [−10.22, −6.34] | −7.56 ms [−10.29, −5.15] | WAL wins for this short write workload despite a slower close. |
| WAL version − DELETE version | −7.19 ms [−10.22, −5.19] | −5.60 ms [−10.11, −4.26] | Same direction with schema fast path. |

The mechanism is visible in the internal timings: DELETE has a much cheaper close (roughly 0.07 ms versus WAL 2.7–2.8 ms), but its flush/update costs about 10 ms more per process in replicate 1; the WAL close penalty does not offset the faster write path. SQLite's own [WAL documentation](https://sqlite.org/wal.html) explains that WAL writes are mostly sequential and that the last connection's close performs a final checkpoint, consistent with the *direction* of these observed timings. This is a mechanistic interpretation, not proof that each millisecond comes from a particular filesystem operation.

## Decision and limits

**Keep WAL.** The proposed DELETE switch is not supported by these measurements: two randomized short-process replicates were 5–8 ms slower externally, and SQLite documents weaker reader/writer concurrency for rollback journals. This does not prove WAL is universally faster; read-mostly workloads, network filesystems (where WAL is unsuitable), and different sync settings are different decisions. See the [official SQLite journal-mode and synchronous pragmas](https://www.sqlite.org/pragma.html) for durability implications; this experiment did **not** relax `synchronous` to gain speed.

**Do not add the `user_version` fast path solely for speed yet.** In the production WAL configuration it saved no measurable external time. Even a small DELETE-only win would not justify schema-version/migration complexity when the mode as a whole loses. A version gate remains valuable for *correct migrations*, a different problem requiring compatibility tests and a documented migration policy; this benchmark does not evaluate that design.

The measured compiler DB path also serializes more spans/metrics, may encounter concurrent readers/writers, and runs under the user's filesystem and antivirus policy. This probe does not test tail behavior under contention or a large accumulated DB. A production change should require a compiler-level paired retest and query/integrity validation, not only this microbenchmark.
