# Compiler architecture: current contract and rationale (2026-09-27)

## Current entry point and scope

One C++23 `compiler` executable compiles one `.sy` translation unit to GNU assembler-compatible **RV64GC / LP64D GNU/Linux** assembly. It contains the frontend, semantic analysis, IR lowering/optimization, code generation, diagnostics, and observability. GCC/binutils and the SysY runtime are needed to link and execute a generated program, not to run the compiler. This fixed target avoids a speculative multi-target abstraction. The course permits ARM *or* RISC-V ([task requirements](../task/上机大作业总体要求.md)); the independently specified “tensor type” has no concrete syntax beyond the implemented multidimensional arrays ([task contract](sysy2022-contract.md)).

The host build and target execution are separate contracts. The CMake option `SYSY_BUILD_RISCV_RUNTIME` is **off by default**; when enabled, CMake checks for a `riscv64*-linux-gnu` GCC toolchain, builds `libs/libsysy/sylib.c` with `-std=c11 -O2 -fcommon -march=rv64gc -mabi=lp64d`, and archives its object as `.cache/build/<preset>/runtime/riscv64-linux-gnu/libsysy.a`. Link a generated program with this archive using a compatible GNU/Linux RISC-V toolchain. The supplied `libs/libsysy/libsysy_riscv.a` is Newlib-linked and is not interchangeable with this glibc-targeted archive. Linux CI is configured for cross-toolchain/QEMU execution; macOS and Windows jobs are configured for host-side tests. Configuration alone is not evidence that every runner has passed. See [build instructions](../build.md) and the [RISC-V ABI note](riscv-abi.md).

**LLVM alternative.** LLVM would reduce backend risk, but its development libraries and distribution/startup cost were not suitable for the initial host environment and short-compile objective. The custom backend is therefore bounded by a verified small IR, explicit ABI handling, and target execution tests. If demonstrated backend maintenance or correctness cost dominates, reconsider LLVM deliberately rather than accumulating ad-hoc target machinery. The [RISC-V psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc) remains the calling-convention authority.

## Data flow and ownership

```text
CLI -> source bytes -> lex/parse -> Program AST -> semantic side tables
    -> typed ModuleIR -> verify / optimize / verify -> RV64 assembly
    -> same-directory temporary file -> rename to requested output
  \-> human diagnostics, help, optional summary -> stderr
  \-> optional structured run/span/event/metric/log -> SQLite
```

`-o -` writes assembly to stdout; ordinary file output is published only after successful compilation. The driver's local objects own one invocation. The AST is not mutated after analysis because `SemanticModel` maps AST-node addresses to stable symbol/type information. `ModuleIR` uses stable integer value/block IDs, distinct terminators, and byte-offset pointer arithmetic. Arrays, globals, and escaped storage remain memory operations; promotable scalar locals can become SSA values. `IrOp::Undef` is a typed **indeterminate** scalar definition, not zero or a stable arbitrary constant. The compiler emits one monolithic executable even though implementation files are modular. Detailed data contracts live in [semantic model](semantic-model.md), [IR lowering](ir-lowering.md), and [optimization](optimization.md).

The parser accepts same-translation-unit **function prototypes** as an implementation extension: a function header can end with `;`, a prototype can omit parameter names, and a later call resolves if a compatible declaration has already appeared. A definition must have parameter names and match earlier declarations; conflicting declarations, duplicate definitions, and user prototypes never defined in the unit are errors. The source-order visibility rule still rejects a forward call without a preceding prototype. Builtin runtime signatures are installed before source traversal. The base language EBNF supplies definitions rather than prototypes; this extension is documented separately from the historical task contract. See `src/parser.cpp`, `src/semantic.cpp`, and the [language reference](../language.md).

Zero-extent arrays are accepted because the task text allows nonnegative dimensions. They have zero logical elements and, when explicitly initialized, accept only `{}`. Lowering reserves one 32-bit physical backing cell solely to represent their address; indexing such an object is out of bounds and has no defined result. Other braced array initializers follow row-major alignment and implicit zero fill. Uninitialized locals do **not** inherit the global zero-initialization rule.

## Optimization and backend

The current pass order is fixed and inspectable:

| Level | Per-function order after input IR verification |
| --- | --- |
| `-O0` | Verification only. |
| `-O1` | Remove unreachable blocks → scalar `mem2reg` → sparse conditional constant propagation (SCCP) → dead-code elimination (DCE) → output IR verification. |
| `-O2` | All `-O1` passes → dominance-scoped common-subexpression elimination (`gvn`) → final DCE → output IR verification. |

`mem2reg` promotes nonescaping four-byte scalar allocas whose addresses only feed direct loads/stores. It inserts Phi nodes at dominance frontiers and begins each promoted slot with a typed `Undef`; thus an uninitialized incoming path is retained instead of being silently replaced with zero. SCCP treats that value as overdefined, CSE never merges it with a defined scalar, and the RISC-V backend does not initialize its stack home. Undefined source behavior remains undefined, while defined paths retain their values. Arrays, escaped pointers, and globals are intentionally not promotable. The optimizer records per-function pass duration and before/after block/instruction counts only when the caller requests stats; this avoids extra timing work on the default short-compile path. `-O2` does **not** claim LICM, inlining, or a generally optimal phase order. See [optimization evidence and limits](optimization.md).

The emitter consumes verified typed IR and implements the RV64GC/LP64D calling convention for scalar and array-address arguments. Its internal stack homes and edge moves are implementation details, not an external IR format. Target execution and differential tests are needed to substantiate ABI and optimization behavior; an IR verifier alone cannot prove generated-program equivalence. See [ABI tests and rationale](riscv-abi.md) and [conformance setup](../testing/conformance.md).

## Diagnostics and observability

Human output—source-positioned diagnostics with excerpts, help, errors, and the optional summary—uses **stderr**. The renderer applies ANSI color only under the selected `auto|always|never` policy. In `auto`, it checks terminal capability and color environment conventions; source text and messages are escaped for terminal safety. Assembly stdout contains no diagnostics. The driver rejects input/output/SQLite path aliases, including potential SQLite `-wal`, `-shm`, and `-journal` sidecars, before compilation.

The SQLite schema currently has **five tables**: `run`, `span`, `event`, `metric`, and `log`. It does *not* have a separate `diagnostics` table; the driver mirrors compiler errors into structured `log` records with stage and location attributes. Records share a run ID and trace ID, and spans carry parent IDs. SQLite uses WAL, a busy timeout, prepared statements, and bounded transaction batches. A mutex serializes queue/database access; there is no lock-free per-thread writer or promise that compiler workers never block. Trace context is passed explicitly, and the CLI can import W3C `traceparent`, but the current compiler is single-process and has no OTLP exporter. When `--db` is omitted, the driver does not open SQLite. `--summary` uses in-memory data, so a database failure does not change compilation status. The human summary separates compilation-stage timings from reported SQLite setup, finish/flush, and close intervals; these are not a complete accounting identity for process wall time. See [observability contract](telemetry.md) and [SQLite WAL constraints](https://www.sqlite.org/wal.html).

## Validation boundaries and live risks

The repository contains host-side frontend/optimizer/telemetry tests and a target conformance harness configured through CTest. These are gates, not evidence of a particular CI run unless its output is inspected. Preserve `-O0/-O1/-O2` differential target tests, ABI fixtures for mixed and overflow arguments, initializer alignment/side-effect cases, prototype source-order cases, and uninitialized-path IR tests. In performance work, measure both tiny-input process startup and long-tail per-pass time before claiming a regression or benefit; the [performance notes](../performance/) contain current experiments.

The important engineering risks remain ABI classification, array/initializer layout, Phi edge lowering, floating corner cases, and telemetry perturbation of short compilations. The architecture is intentionally simple where the dominant SysY workload permits it: fixed target, one IR, fixed pass schedule, and opt-in database. Any later parallel task graph should propagate explicit trace contexts without changing the compiler's existing CLI or output contract.
