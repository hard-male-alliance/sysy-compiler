# SysY black-box conformance plan

## Purpose and oracle

The oracle is `docs/task/上机大作业总体要求.md`, `docs/task/SysY2022语言定义-V1.md`, `docs/task/SysY文法补充说明.md`, and `docs/task/SysY2022运行时库-V1.md`, plus the requested CLI contract (`compiler input.sy -o output.s`). Expected stdout values are calculated by hand from source semantics, never from compiler output. The target is RISC-V64 GNU/Linux assembly. Passing compilation alone proves acceptance and artifact production, **not** correct execution.

Run from any directory with:

```sh
python tests/sysy_conformance.py --compiler /path/to/compiler
```

The harness checks all valid fixtures at `-O0`, `-O1`, and `-O2` by default; `--opt-levels 0` narrows it for diagnosis. It stores outputs only in repository-root `.temp/conformance`. With both `riscv64-linux-gnu-gcc` and `qemu-riscv64` on `PATH`, `--execute auto` cross-compiles the supplied `libs/libsysy/sylib.c`, links generated assembly with it, and checks actual stdout and exit code. The bundled `libsysy_riscv.a` references Newlib `_impure_ptr` and does not link against this Linux glibc toolchain, so source-built runtime is necessary here. `--execute on` makes missing tools an explicit failure; `--execute off` restricts testing to compiler behavior. All children have a 30-second timeout. CTest may run the default command; cross-platform native CI should report target execution as skipped unless it installs the toolchain and emulator.

## Coverage matrix

| Requirement / risk | Positive or negative fixture | Independent expected outcome | Evidence level |
| --- | --- | --- | --- |
| Entry point, runtime `putint`/`putch`, assembly output | `basic.sy` | Accept; `42\n` | Compile + optional target execution |
| Hex/octal literals, const, arithmetic precedence | `integers.sy` | Accept; `30\n` | Compile + optional target execution |
| Nested scopes, shadowing, `while`, `if`, `break`, `continue` | `scopes_loops.sy` | Accept; `1 2 4 5 6 18\n` | Compile + optional target execution |
| Short-circuit and side effects | `short_circuit.sy` | Accept; `62\n` with only two `hit()` calls | Compile + optional target execution |
| Recursion and integer calling convention | `functions.sy` | Accept; `120\n` | Compile + optional target execution |
| Multidimensional partial initialization and array parameter ABI | `arrays.sy` | Accept; `13\n` | Compile + optional target execution |
| Global zero initialization | `global_array.sy` | Accept; `3\n` | Compile + optional target execution |
| Float literal, implicit int-to-float conversion, float compare | `float.sy` | Accept; `1\n` | Compile + optional target execution |
| Float array parameter, loop, float return ABI | `float_array.sy` | Accept; `1\n` | Compile + optional target execution |
| Runtime input ABI | `input.sy` | Input `41\n`; output `42\n` | Compile + optional target execution |
| Line/block comments, `++` as two unary operators, `!` in expression | `comment_unary.sy` | Accept; `5\n` | Compile + optional target execution |
| `-2147483648` and hexadecimal float literal | `number_edges.sy` | Accept; `1\n` | Compile + optional target execution |
| More than eight integer call arguments (stack ABI) | `many_args.sy` | Accept; `55\n` | Compile + optional target execution |
| `else` binds nearest unmatched `if` | `dangling_else.sy` | Accept; `2\n` | Compile + optional target execution |
| Timing builtin lowering and source-line propagation | `timing.sy` | Accept; `6\n`; target stderr contains `Timer@0002-0004` and `TOTAL:` | Assembly symbol + target execution |
| Mixed int/float argument registers and stack overflow | `mixed_args.sy` | Accept; `1\n` | Compile + optional target execution |
| Same-named local object and function remain separately callable | `same_name_call.sy` | Accept; no stdout, exit 8 | Compile + optional target execution |
| Zero-extent global/local arrays with empty initializer | `zero_extent.sy` | Accept; `1\n` | Compile + optional target execution |
| Nested loop-carried values and `break`/`continue` | `nested_phi.sy` | Accept; `243\n` | Compile + optional target execution |
| Runtime-dependent mixed-brace array initializer | `runtime_initializer.sy` | Input `3\n`; output `16\n` | Compile + optional target execution |
| Recursive array argument passing | `recursive_array.sy` | Accept; `10\n` | Compile + optional target execution |
| Negative zero comparison (no undefined division/NaN behavior) | `negative_zero.sy` | Accept; `1\n` | Compile + optional target execution |
| Exact float runtime input/output and floating ABI | `float_io.sy` | Input `0x1.8p0\n`; output `0x1.8p+0\n` per bundled `%a` implementation | GCC runtime oracle + target execution |
| Lexical invalid token and unterminated comment | `bad_token.sy`, `bad_comment.sy` | Reject with located stderr diagnostic, source excerpt and caret; no assembly | Compiler exit/streams |
| Invalid octal literal | `bad_octal.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| Syntax error | `bad_syntax.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| Duplicate declaration, undefined name, const write, invalid break | `bad_duplicate.sy`, `bad_undefined.sy`, `bad_const_write.sy`, `bad_break.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| CLI machine/human stream split and color override | All cases, invalid case with `--color=always/never` | No stdout; ANSI escape only when forced | Compiler exit/streams |
| Untrusted terminal control in command argument | CLI option containing literal ESC | Usage exit 2; no raw ESC reaches stderr | Compiler exit/streams |
| SQLite observability and optional summary | `basic.sy` with `--db` and `--summary` | Valid nonempty SQLite records; human summary on stderr only | Database open and row count |
| Failed compile and pre-existing userspace output | `bad_token.sy` with sentinel assembly file | Nonzero exit; prior file unchanged | File content comparison |
| Successful publish over pre-existing output | `basic.sy` with sentinel assembly file | Exit 0; new nonempty assembly replaces sentinel; stdout remains empty | File content comparison |
| Streaming assembly output | `basic.sy -o - --summary` | Assembly only on stdout; human summary only on stderr | Stream separation |
| Distributed trace parent continuation | `basic.sy` with `--traceparent` and `--db` | Stored run trace ID and remote parent match supplied IDs; root span points to remote parent | SQLite relationship query |
| Database path aliases input/output (data-loss regression) | `basic.sy`, exact and `./` aliases; POSIX symlink/hardlink | Usage exit 2 before source/output contents change | File bytes/text + exit code |

## Deliberate limits and follow-up tests

* The task mentions a “tensor type” but gives no syntax or semantics. This is a **specification gap**, not yet a conformance failure; establish a concrete grammar and ABI before adding tests.
* Assembly is only checked for nonemptiness in compile-only mode. Instruction semantics, RISC-V ABI, array layout, and runtime calls are genuinely verified only when target execution is available. CI should provide at least one such lane.
* General SQLite validation is schema-neutral; the specific `--traceparent` test verifies one remote-to-root relationship, not event completeness, all parent/child span integrity, WAL/concurrency safety, or cross-run retention. Add broader schema-specific assertions as the observability contract matures.
* `--color=auto` terminal detection is not checked: ordinary pipe capture is not a terminal. A PTY test on POSIX and Windows ConPTY test should be added with an established terminal contract.
* The fixed E2E corpus checks behavior at all optimization levels but cannot establish universal optimization semantics or performance thresholds. Broad differential tests against a trusted SysY implementation, IR/assembly structural tests, and reproducible startup/long-tail benchmarks are separate work.
* NaN generation and sign-sensitive negative-zero behavior were not tested because the task does not explicitly establish IEEE-754 exception/rounding guarantees. The `negative_zero.sy` comparison tests only the well-defined equality/ordering behavior also corroborated by C.
* Invalid output should not exist after a failed compile, and the suite verifies preservation of a pre-existing file plus replacement on success. It does not prove atomic replacement under process crash or I/O failure.

## First local execution record

At creation, the repository contained only task documents and runtime libraries, not an executable compiler, CMake target, or test runner. Therefore no product behavior was yet exercised; all cells above are **planned checks**, not passing results. The test script itself was syntax-checked separately.

An independent reference check on WSL Ubuntu-24.04 compiled the initial 14 valid fixtures as C with `gcc -std=c11 -fcommon -Werror=implicit-function-declaration`, injecting `sylib.h`, linking repository `sylib.c`, and replacing the one SysY-only `++5` spelling with its documented C equivalent `+(+5)`. All 14 actual stdout values matched the table and all exited zero. The subsequently added same-name call is a SysY-specific rule (illegal under C shadowing); zero-extent arrays use a GCC extension, so neither is claimed to have an ISO C reference oracle. `-fcommon` is needed because the supplied runtime header defines shared timer globals; the initial reference attempt without it failed at link time with duplicate `_sysy_start` symbols, an environment/library integration issue rather than a SysY test defect. Reference files/executables are in `.temp/conformance`.

The first WSL product run was built using `cmake -S . -B .cache/build/wsl-validation -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON` then `cmake --build .cache/build/wsl-validation -j 4`. Host: Ubuntu-24.04 in WSL, GCC 13.3.0, SQLite 3.45.1, Python 3.12.3, RISC-V GNU/Linux cross-GCC and QEMU user-mode. The prebuilt RISC-V archive link failed for all cases due missing Newlib `_impure_ptr`; after repairing only the test harness to compile the supplied runtime source with `riscv64-linux-gnu-gcc -fcommon -c`, the full initial 14-positive/8-negative `--execute on` suite passed, as did `ctest --test-dir .cache/build/wsl-validation --output-on-failure` (telemetry unit and conformance).

An additional focused QEMU probe executed all initial 14 positives under `-O1` and `-O2`: 28/28 stdout and exit-code comparisons passed. The harness now runs all three optimization levels by default. The timing/mixed-argument fixtures passed at all three levels, including timer source lines on target stderr; after rebuilding the updated semantic/backend sources, same-name call returned 8 and zero-extent arrays produced `1\n` at all three levels. Four newer high-risk fixtures were independently checked against GCC where legal and all passed QEMU at `-O0/-O1/-O2` (12/12 comparisons). The exact float runtime I/O fixture matched the supplied `%a` source-built runtime oracle and passed all three target optimization levels. A focused `--execute off --opt-levels 0` run confirmed source excerpts/carets for eight invalid fixtures, forced/no-color behavior, terminal-control-byte escaping, failed-output preservation, successful-output replacement, stdout assembly/stderr summary separation, and the SQLite remote trace relationship. After a path-collision fix, a dedicated focused run confirmed six database/source/output alias scenarios (exact, `./`, source symlink, output hardlink) all returned 2 and preserved file contents.

Final integrated verification after the telemetry close/summary changes: `cmake --build .cache/build/wsl-validation -j 4 && ctest --test-dir .cache/build/wsl-validation --output-on-failure` passed 2/2 (61.42 s) on WSL Ubuntu-24.04, including all 23 valid programs at three optimization levels under QEMU and eight invalid programs. `cmake --build .cache/build/debug -j 4; ctest --test-dir .cache/build/debug --output-on-failure` passed 2/2 (1.88 s) on Windows MinGW; no RISC-V execution occurred in that lane because the cross-toolchain and emulator were absent. Ninja emitted a recoverable `premature end of file` warning for its build log during the Windows run but completed successfully; this is a local build-cache warning, not a compiler test failure. These are observed fixture-level results, not exhaustive optimizer, ABI, crash-atomicity, or telemetry-concurrency proofs.
