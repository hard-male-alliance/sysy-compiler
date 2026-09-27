# CLI, diagnostics, telemetry, and build review (2026-09-27)

## Scope and method

Reviewed `src/main.cpp`, `src/diagnostics.cpp`, `src/telemetry.cpp`, the public telemetry interface, CLI/telemetry/build contracts, CMake presets, GitHub Actions, and relevant black-box tests. Focused probes used the Debug Windows executable and the WSL Linux executable, with all inputs, outputs, and SQLite databases under `.temp/`. This review does not establish SysY semantic or generated-code correctness and does not substitute for a clean hosted CI run.

## Findings and disposition

| Priority | Finding | Evidence and impact | Disposition |
| --- | --- | --- | --- |
| P1 | The output file could alias the SQLite database. | Before correction, `compiler --db .temp/review_obs_linux_same.s tests/conformance/basic.sy -o .temp/review_obs_linux_same.s --summary` exited **0** on Linux, reported `SQLite saved`, but the destination began `.option nopic` rather than the SQLite header: the assembly rename replaced the live database pathname. The same invocation on Windows exited **1** with `Permission denied`, so behavior also differed by platform. `main.cpp` had checked input/output identity but not DB/output identity. | **Resolved in current source:** `paths_alias()` (`main.cpp:86,194-209`) now checks source, output, DB, and SQLite sidecar paths before telemetry opens. Rebuilt Linux probe rejected exact, `../` relative, symlink, and hardlink aliases with status 2; an output matching `-wal` was also rejected. Regression tests should preserve this invariant. |
| P2 | A failed compiler stage was recorded as a successful span. | Before correction, `bad_token.sy` produced `run.status='error'`, `parse.status='ok'`, and error logs in the same SQLite database. `timed()` let the span destructor use its default `ok` status regardless of the operation result, corrupting failure-oriented trace queries. | **Resolved in current source:** `timed()` (`main.cpp:249-267`) now receives a success predicate and marks error results/exceptions. Rebuilt Windows probe yielded `compiler.run=error`, `parse=error`. Add a persistent black-box assertion so status consistency survives refactoring. |
| P2 | Raw command-line text could inject terminal controls into usage diagnostics. | Before correction, `parse_options()` embedded an unknown argument verbatim and `main()` wrote the error directly to stderr. A focused subprocess probe with `--bad\x1b[2J` returned raw ESC in stderr; a terminal would interpret the clear-screen sequence. | **Resolved in current source:** the driver now applies `escape_terminal_text()` to usage errors, internal failures, telemetry warnings, and summary notes. Rebuilt Windows probe with `--bad\x1b[2J\nspoofed` returned `--bad?[2J?spoofed` with no ESC or injected line break. Machine-readable SQLite fields are left intact. |

### Conditional concern, not a blocking defect

`paths_alias()` is a preflight filesystem check, so another process can swap a path between validation and publication in a directory writable by an adversary. The current code should not claim race-proof protection of the SQLite file; a stronger threat model would require handle-relative or otherwise atomic publication and path ownership rules. For the ordinary single-user local build directory, the new validation fixes the demonstrated failure without adding complex machinery.

## Probe details

- Windows Debug build: `cmake --build --preset debug --parallel 4`; invalid-source probe queried `SELECT name,status FROM span ORDER BY started_unix_ns` using Python `sqlite3`.
- Linux WSL build: `cmake --build .cache/build/wsl-release -j4`; `.temp/review_paths_probe.sh` exercised six alias forms against `.temp/review_paths/a.sy` and `.temp/review_paths/a.s`. All six returned exit status 2 after the correction.
- Control-character probe: Python `subprocess.run([compiler, '--bad' + chr(27) + '[2J' + chr(10) + 'spoofed'], capture_output=True)`; after the correction stderr contained `?` substitutions rather than ESC or an injected line break.

## Build and CI observations

The one-target C++23 CMake layout, explicit SQLite linkage, Debug/Release presets, and Linux/macOS/MSYS2 CI matrix are consistent with the documented cross-platform contract. The conformance runner runs generated binaries only when a RISC-V GNU/Linux toolchain and QEMU are present; the Linux CI job installs both. This review did not execute hosted macOS CI or independently validate every cross-compiler/runtime combination, so their success remains a CI observation rather than a locally established fact.
