# Prototype, mem2reg, and runtime-linking follow-up review

Date: 2026-09-27. Review baseline: `bbafefc`; reviewed the working-tree follow-up changes in the parser, semantic model, lowering, IR optimizer/verifier, RISC-V emitter, CMake/CI, conformance harness, and relevant architecture/build documents. This is an independent change review, not a proof of full SysY conformance.

## Assessment

No demonstrated correctness blocker was found in the reviewed change. The source-order declaration/definition state is represented by a single stable function symbol, with a null `Function::body` distinguishing prototypes; compatible declarations reuse the symbol, and lowering skips prototype-only AST nodes. The uninitialized incoming value of a promotable scalar stack slot is represented as typed `Undef`, treated as overdefined by SCCP, and left uninitialized by the backend. That is conservative for the defined execution paths reviewed; the compiler does not synthesize a zero. The GNU/Linux runtime archive is built from supplied `sylib.c` by an opt-in cross-toolchain target rather than pretending the bundled Newlib archive is glibc-compatible.

## Evidence inspected

- Follow-up diff against `bbafefc`, including `src/{parser,semantic,lower,optimize,riscv}.cpp`, `include/compiler/{ast,semantic,ir,optimize}.hpp`, `CMakeLists.txt`, `.github/workflows/ci.yml`, and `tests/sysy_conformance.py`.
- `cmake --build .cache/build/wsl-validation-prototypes --parallel 4` and `ctest --test-dir .cache/build/wsl-validation-prototypes --output-on-failure` passed **3/3** on WSL Ubuntu; conformance took 64.08 s and reported zero failed assertions. That CMake cache had `SYSY_BUILD_RISCV_RUNTIME=ON`, `/usr/bin/riscv64-linux-gnu-gcc`, and `/usr/bin/riscv64-linux-gnu-ar`. The conformance log contains target-link checks, rather than a compile-only skip.
- Three additional probes under `.temp/review-followup`: compatible `starttime`/`stoptime` builtin prototypes and an unnamed-parameter user prototype compile at `-O2`; an unresolved user prototype is rejected with a source-ranged diagnostic on stderr and exit status 1.
- Existing focused `mem2reg_unit` covers a diamond with one defined and one indeterminate incoming edge at both `-O1` and `-O2`; existing target execution covers the defined branch. This does not prove every CFG shape or every optimization interaction.

## Conditional operational concern (non-blocking)

`SYSY_BUILD_RISCV_RUNTIME` defaults to `OFF`, and `tests/sysy_conformance.py` now requires `--runtime-archive` to execute target programs. Thus a user with GCC and QEMU installed who follows the older generic `cmake --preset debug; ctest --preset debug` flow will get a **passing compile-only** conformance test, not the earlier automatic execution. The script prints `SKIP target execution`, but CTest normally suppresses the output of passing tests; the difference is easy to miss. `docs/build.md` now gives an explicit runtime-enabled WSL command, and Linux CI opts in, so this is primarily a discoverability/coverage issue rather than a defect in the supported runtime-enabled path. The smallest coherent remedy is to conditionally name the CTest case `sysy_conformance_target` when an archive is wired in, and `sysy_conformance_compile_only` otherwise, plus a CMake configure `STATUS` message exposing that mode. Both names retain the `sysy_conformance` prefix for existing `ctest -R` use. Keep the cross toolchain explicitly opt-in on non-Linux hosts; do not silently infer target execution merely from a green CTest count.

## Review limits

I did not run an independent randomized differential test, validate all malformed parser inputs, or test the archive with a Newlib toolchain. No production code or tests were modified by this review.
