# Compile-time performance methodology

## Decision and workloads

The performance question is **user-perceived latency of one `compiler` process producing one assembly file**, not parser throughput in isolation. A second question is which compiler stage dominates a large source, so only measured bottlenecks should motivate production changes. This repository currently has no measured baseline; numbers must not be inferred from design documents or microbenchmarks of another compiler.

`benchmarks/compile_time.py` generates three deterministic SysY inputs:

| Case | What it isolates | Important limitation |
| --- | --- | --- |
| `tiny` | Process launch, initialization, fixed file/output costs | It is *process-cold*, not necessarily disk-cache-cold. |
| `functions` | Many independent functions, calls, locals, and statements; all compilation stages scale | Synthetic repetition may trigger different behavior from hand-written applications. |
| `arrays` | Large global shape, indexed accesses, bounded nested loops | Sparse source for a large static array; does not model massive brace initializers. |

The runner starts a **new compiler process for every sample**, uses `perf_counter_ns()` around subprocess start-to-exit, and redirects streams to `DEVNULL` so terminal rendering is not part of the compile-time result. A `--version` control estimates process load and CLI baseline, but it is **not** a subtractable causal estimate of compiler initialization. Input generation and JSON/SQLite analysis are outside the timed interval. Each workload records the first process separately, discards configurable warm-up runs from the principal distribution, then reports individual samples, median, nearest-rank p95, and a seeded nonparametric 95% bootstrap interval for the median. At low repetition counts the p95 and bootstrap interval are coarse; do not over-interpret small differences. `--telemetry` is a **separate experiment** because SQLite and tracing may perturb startup and stage time. Nested/parallel span durations must never be summed as if they partition wall time.

This is a *warm OS filesystem cache, fresh compiler process* measurement. True cold disk cache cannot be produced portably or without machine-level privileges; saying “cold start” without this distinction is misleading. OS scheduling, antivirus, power modes, dynamic frequency, first DLL load, and CMake build mode can dominate millisecond-scale differences. Compare candidates on the same host, release build, workload hashes, compiler arguments and environment, ideally in alternating A/B blocks when the changes coexist. Use a larger real SysY corpus before shipping an optimization that only helps the synthetic generator.

## Reproduction

```sh
cmake --preset release
cmake --build --preset release --parallel
python benchmarks/compile_time.py --compiler .cache/build/release/compiler --repetitions 31
# Measure the separate tracing/SQLite mode (--db=<path>):
python benchmarks/compile_time.py --compiler .cache/build/release/compiler --telemetry --repetitions 31
# Interleaved AB/BA estimate of the end-to-end SQLite/tracing overhead:
python benchmarks/telemetry_pair.py --compiler .cache/build/release/compiler --repetitions 31
# No-database in-process duration from the optional human summary:
python benchmarks/summary_probe.py --compiler .cache/build/release/compiler --repetitions 31
# With SQLite lifecycle timing; repeat with --case functions for medium inputs:
python benchmarks/summary_probe.py --compiler .cache/build/release/compiler --db --repetitions 31
# Larger long-tail input:
python benchmarks/compile_time.py --compiler .cache/build/release/compiler --functions 400 --statements 32 --repetitions 15
```

On Windows use the actual executable path, normally `.cache/build/release/compiler.exe`. All generated `.sy`, `.s`, `.sqlite`, and `results.json` files remain under `.temp/performance`. The report records OS, processor string, CPU count, Python version, executable/input SHA-256, command, all raw samples, and available SQLite spans. It deliberately does not claim to measure CPU time or peak memory. If telemetry is requested, verify that the CLI actually creates the database; an absent database is not evidence of zero stage time.

## Evidence and interpretation guardrails

- [LLVM benchmarking guidance](https://llvm.org/docs/Benchmarking.html) emphasizes repeated measurements, controlled noise, and representative inputs. [Clang's time-trace documentation](https://clang.llvm.org/docs/UsersManual.html#options-to-emit-resource-consumption-reports) shows compiler-stage tracing in a production compiler and notes trace verbosity/data-volume trade-offs.
- [pyperf documentation](https://pyperf.readthedocs.io/en/latest/) illustrates why distributions, warm-ups, and system configuration matter. Our lightweight Python runner is not a replacement for controlled system tuning; it is a reproducible project-local baseline.
- [Huemer et al., ECOOP 2024](https://doi.org/10.4230/LIPIcs.ECOOP.2024.20) use fine-grained, outlier-driven compilation-time analysis in GraalVM. The relevant transferable idea is to inspect *where* tail latency occurs, not to assume their reported speedup or their JIT bottleneck applies to this ahead-of-time SysY compiler.

**Decision rule:** prioritize a change only if repeated, equivalent release runs show a meaningful end-to-end win for the stated workload, without semantic or output regressions, and without worse p95/telemetry overhead disproportionate to the gain. A fast isolated pass does not establish a faster compiler. Stage spans are diagnostic evidence; external wall time is the user's actual latency.
