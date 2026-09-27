# SysY black-box conformance plan

## Purpose and oracle

The oracle is `docs/task/上机大作业总体要求.md`, `docs/task/SysY2022语言定义-V1.md`, `docs/task/SysY文法补充说明.md`, and `docs/task/SysY2022运行时库-V1.md`, plus the requested CLI contract (`compiler input.sy -o output.s`). Expected stdout values are calculated by hand from source semantics, never from compiler output. The target is RISC-V64 GNU/Linux assembly. Passing compilation alone proves acceptance and artifact production, **not** correct execution.

Run from any directory with:

```sh
python tests/sysy_conformance.py --compiler /path/to/compiler \
  --runtime-archive .cache/build/wsl-validation-prototypes/runtime/riscv64-linux-gnu/libsysy.a \
  --execute on
```

The harness checks all valid fixtures at `-O0`, `-O1`, and `-O2` by default; `--opt-levels 0` narrows it for diagnosis. It stores outputs only in repository-root `.temp/conformance`. With both `riscv64-linux-gnu-gcc` and `qemu-riscv64` on `PATH`, and a matching archive supplied via `--runtime-archive`, target mode statically links generated assembly and checks actual stdout and exit code. Configure CMake with `-DSYSY_BUILD_RISCV_RUNTIME=ON` to build the matching GNU/Linux archive at `<build>/runtime/riscv64-linux-gnu/libsysy.a`; CTest then registers **`sysy_conformance_target`** with `--execute on` and the archive path. Without that option, CTest registers **`sysy_conformance_compile_only`** with `--execute off`. The configure status and `ctest -N` therefore expose which capability is actually tested; a missing QEMU/archive cannot silently downgrade the target lane. The bundled `libsysy_riscv.a` references Newlib `_impure_ptr` and does not link against the Linux glibc toolchain. The validator no longer works around this by compiling `sylib.c` itself; the build target owns the matching archive. For manual runs, `--execute on` makes missing tools/archive an explicit failure, `--execute off` restricts testing to compiler behavior, and `--execute auto` reports any skip explicitly. All children have a 30-second timeout.

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
| Forward call via earlier prototype | `proto_forward.sy` | Accept; `42\n` | Compile + target execution |
| Mutual recursion via prototypes | `proto_mutual.sy` | Accept; `2\n` | Compile + target execution |
| Compatible repeated named/unnamed prototype parameters | `proto_repeated.sy` | Accept; `42\n` | Compile + target execution |
| Multidimensional array prototype signature with unnamed prototype formal | `proto_array.sy` | Accept; `6\n` | GCC reference + target execution |
| `f(void)` prototype and definition | `proto_void_parameter.sy` | Accept; `42\n` | Compile + target execution |
| Partly uninitialized local, defined executed path | `mem2reg_defined_path.sy` | Accept; `7\n` at all levels; do not execute false branch | Target execution + independent IR unit test |
| Lexical invalid token and unterminated comment | `bad_token.sy`, `bad_comment.sy` | Reject with located stderr diagnostic, source excerpt and caret; no assembly | Compiler exit/streams |
| Invalid octal literal | `bad_octal.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| Syntax error | `bad_syntax.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| Duplicate declaration, undefined name, const write, invalid break | `bad_duplicate.sy`, `bad_undefined.sy`, `bad_const_write.sy`, `bad_break.sy` | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| Prototype signature/shape mismatch, duplicate definition, unresolved prototype, main only declared | Five `bad_proto_*.sy` fixtures | Reject with located stderr diagnostic; no assembly | Compiler exit/streams |
| CLI machine/human stream split and color override | All cases, invalid case with `--color=always/never` | No stdout; ANSI escape only when forced | Compiler exit/streams |
| Untrusted terminal control in command argument | CLI option containing literal ESC | Usage exit 2; no raw ESC reaches stderr | Compiler exit/streams |
| SQLite observability and optional summary | `basic.sy` with `--db` and `--summary` | Valid nonempty SQLite records; human summary on stderr only | Database open and row count |
| Prototype semantic failure observability | `bad_proto_mismatch.sy --db ... --summary --color=never` | Located stderr diagnostic + summary, no output, error run/semantic span/error log | CLI + SQLite records |
| Optimizer telemetry | `mem2reg_defined_path.sy -O1 --db ...` | `compiler.pass.duration` metrics include `mem2reg` | SQLite metric attributes |
| Failed compile and pre-existing userspace output | `bad_token.sy` with sentinel assembly file | Nonzero exit; prior file unchanged | File content comparison |
| Successful publish over pre-existing output | `basic.sy` with sentinel assembly file | Exit 0; new nonempty assembly replaces sentinel; stdout remains empty | File content comparison |
| Streaming assembly output | `basic.sy -o - --summary` | Assembly only on stdout; human summary only on stderr | Stream separation |
| Distributed trace parent continuation | `basic.sy` with `--traceparent` and `--db` | Stored run trace ID and remote parent match supplied IDs; root span points to remote parent | SQLite relationship query |
| Database path aliases input/output (data-loss regression) | `basic.sy`, exact and `./` aliases; POSIX symlink/hardlink | Usage exit 2 before source/output contents change | File bytes/text + exit code |

## Deliberate limits and follow-up tests

* The task mentions a “tensor type” but gives no syntax or semantics. This is a **specification gap**, not yet a conformance failure; establish a concrete grammar and ABI before adding tests.
* Function prototypes are a repository extension motivated by the course summary's “function declarations” requirement; the detailed SysY EBNF has only function definitions. The five positive and five negative prototype fixtures test the chosen self-contained translation-unit contract, not a claim that every external SysY implementation accepts these forms.
* Assembly is only checked for nonemptiness in compile-only mode. Instruction semantics, RISC-V ABI, array layout, and runtime calls are genuinely verified only when target execution is available. CI should provide at least one such lane.
* General SQLite validation is schema-neutral; the specific `--traceparent` test verifies one remote-to-root relationship, not event completeness, all parent/child span integrity, WAL/concurrency safety, or cross-run retention. Add broader schema-specific assertions as the observability contract matures.
* `--color=auto` terminal detection is not checked: ordinary pipe capture is not a terminal. A PTY test on POSIX and Windows ConPTY test should be added with an established terminal contract.
* The fixed E2E corpus checks behavior at all optimization levels but cannot establish universal optimization semantics or performance thresholds. Broad differential tests against a trusted SysY implementation, IR/assembly structural tests, and reproducible startup/long-tail benchmarks are separate work.
* NaN generation and sign-sensitive negative-zero behavior were not tested because the task does not explicitly establish IEEE-754 exception/rounding guarantees. The `negative_zero.sy` comparison tests only the well-defined equality/ordering behavior also corroborated by C.
* The `mem2reg_defined_path.sy` fixture contains an indeterminate return on the false branch; it calls the function only with a true flag. The standalone `mem2reg_unit` constructs both CFG paths but checks representation/invariants, never treats the false-path value as a deterministic output. It asserts that typed `Undef` survives SCCP rather than becoming a concrete constant.
* Invalid output should not exist after a failed compile, and the suite verifies preservation of a pre-existing file plus replacement on success. It does not prove atomic replacement under process crash or I/O failure.

## First local execution record

At creation, the repository contained only task documents and runtime libraries, not an executable compiler, CMake target, or test runner. Therefore no product behavior was yet exercised; all cells above are **planned checks**, not passing results. The test script itself was syntax-checked separately.

An independent reference check on WSL Ubuntu-24.04 compiled the initial 14 valid fixtures as C with `gcc -std=c11 -fcommon -Werror=implicit-function-declaration`, injecting `sylib.h`, linking repository `sylib.c`, and replacing the one SysY-only `++5` spelling with its documented C equivalent `+(+5)`. All 14 actual stdout values matched the table and all exited zero. The subsequently added same-name call is a SysY-specific rule (illegal under C shadowing); zero-extent arrays use a GCC extension, so neither is claimed to have an ISO C reference oracle. `-fcommon` is needed because the supplied runtime header defines shared timer globals; the initial reference attempt without it failed at link time with duplicate `_sysy_start` symbols, an environment/library integration issue rather than a SysY test defect. Reference files/executables are in `.temp/conformance`.

The first WSL product run was built using `cmake -S . -B .cache/build/wsl-validation -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON` then `cmake --build .cache/build/wsl-validation -j 4`. Host: Ubuntu-24.04 in WSL, GCC 13.3.0, SQLite 3.45.1, Python 3.12.3, RISC-V GNU/Linux cross-GCC and QEMU user-mode. The prebuilt RISC-V archive link failed for all cases due missing Newlib `_impure_ptr`; after repairing only the test harness to compile the supplied runtime source with `riscv64-linux-gnu-gcc -fcommon -c`, the full initial 14-positive/8-negative `--execute on` suite passed, as did `ctest --test-dir .cache/build/wsl-validation --output-on-failure` (telemetry unit and conformance).

An additional focused QEMU probe executed all initial 14 positives under `-O1` and `-O2`: 28/28 stdout and exit-code comparisons passed. The harness now runs all three optimization levels by default. The timing/mixed-argument fixtures passed at all three levels, including timer source lines on target stderr; after rebuilding the updated semantic/backend sources, same-name call returned 8 and zero-extent arrays produced `1\n` at all three levels. Four newer high-risk fixtures were independently checked against GCC where legal and all passed QEMU at `-O0/-O1/-O2` (12/12 comparisons). The exact float runtime I/O fixture matched the supplied `%a` source-built runtime oracle and passed all three target optimization levels. A focused `--execute off --opt-levels 0` run confirmed source excerpts/carets for eight invalid fixtures, forced/no-color behavior, terminal-control-byte escaping, failed-output preservation, successful-output replacement, stdout assembly/stderr summary separation, and the SQLite remote trace relationship. After a path-collision fix, a dedicated focused run confirmed six database/source/output alias scenarios (exact, `./`, source symlink, output hardlink) all returned 2 and preserved file contents.

Final integrated verification after the telemetry close/summary changes: `cmake --build .cache/build/wsl-validation -j 4 && ctest --test-dir .cache/build/wsl-validation --output-on-failure` passed 2/2 (61.42 s) on WSL Ubuntu-24.04, including all 23 valid programs at three optimization levels under QEMU and eight invalid programs. `cmake --build .cache/build/debug -j 4; ctest --test-dir .cache/build/debug --output-on-failure` passed 2/2 (1.88 s) on Windows MinGW; no RISC-V execution occurred in that lane because the cross-toolchain and emulator were absent. Ninja emitted a recoverable `premature end of file` warning for its build log during the Windows run but completed successfully; this is a local build-cache warning, not a compiler test failure. These are observed fixture-level results, not exhaustive optimizer, ABI, crash-atomicity, or telemetry-concurrency proofs.

## Prototype, mem2reg, and matching-runtime follow-up

The follow-up added five legal prototype programs, five rejected prototype programs, and one defined-path mem2reg program. For the first ten, the oracle is the newly documented repository extension: a prototype permits an unnamed parameter, compatible redeclarations are allowed, an eventual matching definition is required for every user function, and `main` must be defined. The mutual-recursion and array outputs were computed from source semantics; only legal programs are executed. The mem2reg case has one uninitialized CFG path but calls only the initialized path; its expected output is `7\n`. An independent C++ IR test in `tests/mem2reg_test.cpp` constructs a partial-initialization diamond, runs `optimize()` at levels 1 and 2, then asserts no remaining scalar `Alloca`/`Load`/`Store`, exactly one typed `Undef`, a two-input `Phi` with an `Undef` incoming edge, return of the `Phi`, and a clean `verify_ir()` result. It does not infer the optimizer's correctness solely from target output.

On WSL Ubuntu-24.04, configuration/build used:

```sh
cmake -S . -B .cache/build/wsl-validation-prototypes -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSYSY_BUILD_RISCV_RUNTIME=ON
cmake --build .cache/build/wsl-validation-prototypes -j 4
ctest --test-dir .cache/build/wsl-validation-prototypes --output-on-failure
```

The built archive is `.cache/build/wsl-validation-prototypes/runtime/riscv64-linux-gnu/libsysy.a`. A focused archive-link/QEMU probe executed the six new legal programs at `-O0/-O1/-O2`: **18/18** observed stdout and exit values matched expectations, proving that this test lane uses a real static archive rather than a test-generated object. All five prototype positives were independently compiled by host GCC C11 against the supplied `sylib.c`; their reference stdout values matched the fixture oracle. The array prototype was then strengthened to omit both formal names in the declaration; a focused QEMU `-O1` run still produced `6\n`. The first compiled binary accepted `bad_proto_undefined.sy` despite an unresolved call; after the semantic end-of-unit check was added and the binary rebuilt, the same fixture was rejected with a located “declared but not defined” error and no assembly. This was a genuine pre-fix implementation gap, not a test-harness failure. A focused compile-only check verified that a mismatched prototype with `--db --summary --color=never` stores `run.status=error`, `span(name=semantic).status=error`, and an error log while keeping the located diagnostic and summary on stderr; a successful `-O1 --db` run stored eight `compiler.pass.duration` rows, including `mem2reg`.

Final integrated follow-up verification after all fixtures and those observability assertions: `cmake --build .cache/build/wsl-validation-prototypes -j 4 && ctest --test-dir .cache/build/wsl-validation-prototypes --output-on-failure` passed **3/3** in 65.54 s on WSL Ubuntu-24.04. `sysy_conformance` covered **29 legal programs × 3 optimization levels** under QEMU using the matching archive, **13 rejected programs**, and all existing diagnostics/SQLite/traceparent/path/stream assertions. `mem2reg_unit` passed separately within the same CTest run. On Windows MinGW, `cmake --build .cache/build/debug -j 4; ctest --test-dir .cache/build/debug --output-on-failure` passed **3/3** in 2.59 s, including compile-only conformance and IR mem2reg unit; RISC-V execution was correctly skipped. Windows Ninja again printed a recoverable `.ninja_log` warning but the build and all tests exited zero. This verifies the stated cases, not all possible prototype signatures, IR graphs, or target executions.

### CTest mode-visibility follow-up

The integrated results above predate the CTest name split, when both modes were named `sysy_conformance`; the harness and test content were not changed by the split. After CMake reconfiguration, WSL status reported target execution enabled with the exact archive path, and `ctest --test-dir .cache/build/wsl-validation-prototypes -N` listed `mem2reg_unit`, `telemetry_unit`, and **`sysy_conformance_target`**. A focused `ctest -R mem2reg_unit --output-on-failure` passed 1/1. Windows `cmake --preset debug` reported target execution disabled; `ctest --test-dir .cache/build/debug -N` listed `mem2reg_unit`, `telemetry_unit`, and **`sysy_conformance_compile_only`**. A focused `ctest -R sysy_conformance_compile_only --output-on-failure` passed 1/1 (2.47 s). The 29 × 3 QEMU corpus was not rerun merely to verify the CTest naming change.
