# Compiler architecture decision (2026-09-27)

## Scope and decision

Build one C++23 `compiler` executable for one `.sy` translation unit, emitting GNU assembler-compatible RISC-V 64-bit assembly for Linux, using the RV64GC ISA and LP64D ABI. The executable contains all front-end, optimization, assembly-emission, diagnostics, and observability code; GCC/binutils and the supplied SysY runtime source are *link/test tools*, not subprocesses needed to compile. Linkage and execution are checked with `riscv64-linux-gnu-gcc` and QEMU on Linux. The supplied `libsysy_riscv.a` depends on Newlib and is incompatible with this glibc toolchain, so tests rebuild `sylib.c` with the selected cross-compiler. Windows/macOS CI is configured to build and exercise host-independent front-end/IR tests; Linux cross-toolchain CI is configured for real RISC-V end-to-end tests. The target is deliberately fixed first: a generic target abstraction would add branches to every representation before a second target is known.

This decision follows `docs/task/上机大作业总体要求.md`: target ARM *or* RISC-V; full SysY 2022 int/float, arrays/tensors, optimization, and linking to the runtime. `docs/task/SysY文法补充说明.md` overrides the base grammar: `!` is accepted anywhere in expressions, parameter dimensions after `[]` are constant expressions, `-2147483648` must work, and `putf` is not required. The supplied `libs/libsysy/sylib.h` confirms `starttime` and `stoptime` are macros lowering to `_sysy_starttime(__LINE__)` and `_sysy_stoptime(__LINE__)`; the compiler must mirror this line-aware call contract.

**Why custom code generation rather than embedded LLVM?** LLVM already solves SSA and code generation and would lower implementation risk, but LLVM development libraries are not available in the local Windows environment, static distribution is large, and early process startup is a requirement. A custom narrow backend is credible only with strict representation boundaries, a small ABI planner, oracle-based differential tests against Clang, and executable RISC-V CI. If backend correctness or optimization work consistently exceeds this budget, reconsider LLVM as an explicit architectural change, not an ad-hoc fallback. [LLVM documents the available RISC-V backend and `llc` output contract](https://llvm.org/docs/RISCVUsage.html), while the [RISC-V psABI is the authority for calling convention](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc).

## Dominant workflow and ownership

```text
argv -> Driver -> SourceFile -> Lexer -> Parser -> AST
                                         -> SemanticAnalyzer -> SemanticModel
                                         -> IRBuilder -> ModuleIR
                                         -> Verifier -> Optimizer -> Verifier
                                         -> RiscvEmitter -> temp .s -> atomic rename
              \-> DiagnosticEngine -> stderr (human-oriented, optional ANSI)
              \-> RunRecorder (spans/events/counters) -> SQLite transaction
              \-> SummaryRenderer -> stderr, only when requested
```

No pass writes to stdout by default. `-o -` reserves stdout for assembly alone; help, diagnostics and optional summary go to stderr. Failure never leaves a partially replaced output file. The driver owns a per-invocation `RunContext` and all mutable compilation state; components receive explicit references. This makes future per-function parallelism possible without shared global compiler state. Each component reports `expected<T, Error>` (or an equivalent result type), and recovery is limited to lexer/parser/semantic diagnostics; codegen and verifier failures stop artifact publication.

| State / owner | Representation and invariant | Lifetime |
|---|---|---|
| `SourceFile` | Immutable source bytes, path, and line index; existing `SourceRange` has half-open byte offsets plus one-based line/column. | Entire invocation; all diagnostic spans borrow it. |
| `Lexer` | Tokens preserve source ranges and literal spelling. Integers retain magnitude wide enough to recognize `2147483648` before unary minus. | Through parsing. |
| `Parser` | Existing `sysy::Program` AST (`include/compiler/ast.hpp`) owns expression/statement nodes through `shared_ptr`, preserving source order, spelling, and ranges but making no type claims. One expression grammar includes `!`, `&&`, `||`. | Until typed lowering. |
| `SemanticAnalyzer` | `SemanticModel` side tables keyed by stable AST object identity; stacked lexical scopes; type is scalar `i32`/`f32`/`void` or array with shape. Function signatures are separate from variable lookup if required by SysY name rules. Every expression has a typed result and conversion plan. The AST is immutable after parsing so its node addresses remain stable. | Through IR construction. |
| `SemanticModel` | Existing `semantic.hpp` defines stable `Symbol` identities, typed expression/reference side tables, and flattened `InitSlot` vector; omitted elements are zero. Semantic errors accumulate in `SemanticResult.errors`. | IR construction. |
| `ModuleIR` | Index-addressed typed basic blocks, instructions, values, globals and constant data; each block has exactly one terminator, phi nodes precede normal instructions, IDs never dangle. Memory remains explicit for arrays/globals/escaped storage. | Optimization and emission. |
| `RiscvEmitter` | `FramePlan` assigns homes to stack objects, spills and outgoing stack arguments; `CallPlan` assigns each argument independently to ABI integer/floating registers or stack. | One function at a time. |
| `RunRecorder` | Immutable trace IDs/context carried with tasks, append-only in-memory records, one writer flushes to SQLite. | Entire invocation, then flush. |

Representative module interfaces. Parser and semantic signatures below are already defined in `include/compiler/parser.hpp` and `include/compiler/semantic.hpp`; later signatures are provisional. They must adapt to the existing AST and results rather than replacing them:

```cpp
struct CompileOptions { Target target; OptLevel opt; OutputPath output; ObservabilityOptions observability; };
struct CompileResult { bool success; std::size_t diagnostics; RunSummary summary; };
CompileResult compile(const CompileOptions&, SourceFile&, DiagnosticEngine&, Telemetry&);

ParseResult parse(std::string_view source);
SemanticResult analyze(const Program&);
std::expected<ModuleIR, LoweringError> lower(const Program&, const SemanticModel&);
std::expected<void, IrError> verify(const ModuleIR&);
void optimize(ModuleIR&, OptLevel, PassObserver&);
std::expected<void, CodegenError> emit_rv64(const ModuleIR&, std::ostream&);
```

The production implementation can keep these private to libraries/headers; there is only one delivered executable. The API explicitly separates *diagnostic success* from *output publishing*. At `-O0`, correctness does not depend on passes. `-O1/+` runs mandatory promotion/Cfg cleanup and selected scalar optimizations. A request for IR dumping is a separate artifact/stderr mode and never mixed into assembly stdout.

### Concrete IR data contract for parallel implementation

The initial IR should be **one simple, typed, index-addressed representation**, not a class hierarchy. `ValueId` and `BlockId` are `uint32_t` indices, with an explicit invalid sentinel. `IrType` has `I32`, `F32`, `Ptr`, `Void` (pointers are internal only). `FunctionIR` stores a signature, `vector<BasicBlock>`, entry block, and value-type table. `BasicBlock` stores a vector of `Instruction` and one distinct `Terminator`. `ModuleIR` stores globals (name, element type, byte size/alignment, constant bytes/zero fill) and functions. Stable IDs enable diagnostics, SSA verification, and backend indexing while vectors can move. Do not store raw pointers into instruction vectors.

```cpp
enum class IrType { I32, F32, Ptr, Void };
enum class IrOp {
    ConstI32, ConstF32, Param, Alloca, GlobalAddr, PtrAdd,
    Load, Store, Add, Sub, Mul, Div, Rem, Cmp,
    IToF, FToI, Call, Phi
};
struct Instruction {
    IrOp op;
    IrType type;                // result type, or Void for Store/void Call
    ValueId dst;                // invalid if there is no result
    std::vector<ValueId> args; // positional operands
    std::int64_t imm = 0;      // integer bits, f32 bit pattern, alloca bytes, etc.
    std::string symbol;        // only GlobalAddr/Call
    std::vector<std::pair<BlockId, ValueId>> incoming; // only Phi
    SourceRange range;
};
struct Terminator { TermKind kind; ValueId cond_or_return; BlockId yes; BlockId no; };
```

`PtrAdd` takes a base pointer and **byte** offset value; array stride multiplication has already happened in the lowerer. Comparisons have a predicate code in `imm`, their result is `I32` 0/1. `Alloca` carries byte count and alignment (two integer fields or a small payload), not an AST type. Calls keep source-order value operands and symbol name; ABI assignment occurs only in the backend. Blocks are complete before verification and emission, including dead code handling. If a more strongly typed `std::variant` payload is easier to implement, it may replace generic fields *without changing those invariants*.

## Language semantics that must be modeled once

1. **Scalar types:** `int` is signed 32-bit and `float` IEEE binary32. Casts are explicit IR instructions after semantic analysis. Arithmetic and comparisons use scalar type, with `i32` to `f32` promotion where the language requires it; remainder is integer-only. `!` and conditions use zero/nonzero truthiness; `&&`/`||` lower to control-flow branches, never eager arithmetic. For floating NaN, nonzero truthiness must match C-style `x != 0.0f`. Undefined out-of-range float-to-int conversion and integer division by zero need not be made deterministic; optimization must not invent new observable behavior for defined programs.
2. **Integer literal boundary:** Lex decimal/octal/hex magnitude independently from sign, then semantic constant folding accepts the unary-negated `2147483648` as `INT32_MIN`; other out-of-range literals get a source-spanned diagnostic. Do not squeeze into `int32_t` in the lexer.
3. **Arrays:** `ArrayType {element, dimensions}` has known positive extents in declarations; parameter type has omitted first extent and known trailing extents. Source index `a[i][j]` lowers by row-major strides. An incompletely indexed array may decay to a pointer only where an array argument is expected. Internal pointer type is *not* a SysY user type. All total-size and byte-offset multiplication is overflow-checked during semantic analysis.
4. **Initializers:** One brace-aware flattening algorithm consumes scalar entries in declaration order, aligns nested braces to the current subarray boundary, rejects excess elements, and emits implicit zero fills. Global/const initializers require compile-time values. Local variable initializers retain ordered runtime expression evaluation for explicit elements; zero-fills do not reorder side effects. Uninitialized locals remain unspecified; uninitialized globals are zero.
5. **Scopes and declarations:** Declaration becomes visible from its declaration point; local shadowing is allowed only for non-overlapping scopes. Calls must resolve according to source-order visibility. Runtime library signatures are injected as builtin function symbols, not parsed as fake source text. Exactly one `int main()` is required.
6. **Calls and timing:** Scalar arguments by value; array arguments as start address. `getarray`/`putarray` receive pointers without bounds metadata. Timing intrinsics, if provided through macro-like wrappers in `sylib.h`, need call-site source-line integer arguments and exact symbol spelling confirmed from that header/library.

## IR, optimization, and backend

The IR is SSA for register-like scalar values by construction; mutable local variables start as `alloca`/`load`/`store` so promotion is inspectable as a separate pass. A verifier checks type agreement, terminators, predecessor lists, dominance of uses, phi incoming edge agreement, and valid memory element types. Dominator-frontier or sealed-block SSA construction may implement `mem2reg`; for an educational compiler, use a well-scoped mem2reg pass on promotable scalar allocas and keep arrays/globals/escaped addresses in memory. This is the meaningful interpretation of “complete mem2reg”: all *promotable* allocas, including loop/diamond cases, not the impossible promotion of arbitrary aliased arrays. LLVM describes the same distinction in its [pass reference](https://llvm.org/docs/Passes.html#mem2reg-promote-memory-to-register).

`-O1`: mem2reg -> simplify CFG/unreachable elimination -> sparse conditional constant propagation (SCCP) -> dead-code elimination (DCE) -> verify. Add global value numbering (GVN) or scalar loop-invariant code motion (LICM) as the second elective optimization only after its differential and metamorphic tests pass; a simple local expression CSE is not a substitute for the required whole-function optimization claim. Make pass order explicit and record per-pass instruction/block deltas. Research on phase ordering demonstrates a genuine search problem, but learned pass policies are not yet justified for this small compiler; fixed transparent pipelines are more reproducible and cheaper to launch. [LLVM New Pass Manager](https://llvm.org/docs/NewPassManager.html), [compiler phase-ordering research example](https://arxiv.org/abs/1901.04615).

Backend lowering happens in two steps: (1) `FramePlan` and edge-move plan after phi destruction (critical edges split first), and (2) instruction selection/assembly printing. At first, every SSA value may have a stable stack home and instructions may use caller-saved scratch registers; this is a correctness baseline, **not** an acceptable final hot-loop performance claim. A bounded per-basic-block register cache/allocator can remove redundant loads/stores without changing the IR or ABI planner. The frame is 16-byte aligned at calls; stack parameters use psABI locations; integer and floating argument register allocation are independent **until f32 register exhaustion, then f32 follows integer calling convention**; 32-bit ints are sign-extended according to LP64D; `float` calls/returns use `fa*`; used `s*`, `fs*` (under LP64D), and `ra` are preserved whenever required. Large stack offsets, globals, data relocations, recursion, and calls with >8 integer/float parameters need explicit tests. [RISC-V psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc).

Assembly publication is deterministic for the same input/options, excluding telemetry IDs/timestamps, and uses safe symbol labels plus `.rodata`, `.data`, `.bss`, `.text` as appropriate. RISC-V assembler pseudoinstructions are fine if GNU assembler accepts them. Runtime library linked under Linux with the supplied archive; do not copy its implementation into compiler code.

## Diagnostics and observability

**Human path:** `Diagnostic {severity, code, primary Span, message, labels[], notes[], fixits[]}`. A renderer shows `path:line:column`, a clipped source excerpt/caret, secondary labels, and contextual notes. Color policy is `auto|always|never`: `auto` checks that stderr is a TTY and supports ANSI (Windows virtual-terminal mode where available), honors `NO_COLOR`, and never inserts escape sequences into SQLite, redirected stderr, or generated assembly. Errors, warnings, help and optional summary all go to stderr; stdout is reserved for requested machine artifact. Compile failure exits nonzero but still flushes telemetry if enabled.

**Machine path:** one `run_id` is the join key across `runs`, `events`, `spans`, `metrics`, and `diagnostics` tables, schema-versioned with `PRAGMA user_version`. A span is `{trace_id[16], span_id[8], parent_span_id?, run_id, name, start_ns, end_ns, status, attrs}`. Events carry UTC timestamp, monotonic offset, severity, component, code, structured attributes. Metrics include input bytes, token/AST/IR counts, stage durations, peak resident memory if portable measurement exists, output bytes, pass deltas, and exit status. DB access uses prepared statements and one transaction per run, optionally WAL for concurrent readers. A single writer drains bounded per-thread buffers; compiler workers never block on SQLite. WAL still permits only one writer and is unsuitable for network filesystems, so busy timeout and a clear write-failure policy are required. [SQLite WAL documentation](https://www.sqlite.org/wal.html).

Tracing context is explicit `{trace_id, parent_span_id, flags}` rather than thread-local-only. The root span covers compile invocation; child spans cover lex/parse/semantic/lower/pass/codegen/publish. Future parallel tasks receive immutable context and create child spans; cross-process work can propagate W3C `traceparent`. This is *distributed-ready*, not a claim that the current single process performs distributed compilation. Spans close via RAII even on errors. High-cardinality source paths/identifiers and source text are excluded or opt-in to avoid privacy and DB bloat. [OpenTelemetry Trace API](https://opentelemetry.io/docs/specs/otel/trace/api/), [W3C propagation requirements](https://opentelemetry.io/docs/specs/otel/context/api-propagators/).

The default compilation path should not open SQLite or initialize a heavyweight telemetry runtime unless telemetry is enabled; lightweight stage timing can still power `--summary`. `--summary` renders from in-memory counters rather than querying the DB, so a DB outage does not suppress the human result. Recommended DB failure behavior: compilation output remains valid, a warning is emitted on stderr, and observability loss is reflected in summary/exit metadata; a `--telemetry-required` mode can deliberately fail the invocation for CI audit uses. This policy must be finalized with user-visible CLI tests.

## Delivery slices and executable gates

| Slice | Deliverable | Gate |
|---|---|
| 1 | CMake presets, CLI, source manager, lexer/parser, diagnostic renderer | Cross-platform build; golden lexical/parser/diagnostic tests; stdout/stderr and color tests. |
| 2 | Typed AST, constants, initializer normalization, builtins | Scope/type/shape/constant evaluator tests; all grammar corrections. |
| 3 | Verified IR and `-O0` RV64 emitter | GCC assemble/link/QEMU run for scalar, calls, control flow, globals, arrays, float, mixed ABI, library calls. |
| 4 | SSA and optimization passes | IR verifier; pass-specific golden tests; differential execution `-O0` vs `-O1` plus C/Clang oracle where semantics match. |
| 5 | SQLite recording, tracing, summary and microbenchmarks | DB schema/foreign-key tests; concurrent span parentage; corrupted/unwritable DB isolation; startup and hot-function benchmarks. |

Quality evidence should include golden diagnostics, property/fuzz tests for parser and initializer alignment, generated-program differential tests, ABI fixtures compiled with GCC/Clang, sanitizers on host tests, and QEMU execution using `libsysy_riscv.a`. Measure both cold process latency for tiny inputs and per-stage long-tail behavior for large arrays/functions; set regression thresholds only after a clean baseline. Architecture risks ranked: (1) ABI misclassification of mixed/int/float/overflow args, (2) initializer brace alignment and side effects, (3) phi destruction on critical edges, (4) numerical corner cases, (5) instrumentation perturbing short compilation. Each has an independent focused executable gate above. Do not claim completion from unit tests alone.

## Open assumptions to verify early

- Confirm the RISC-V archive exports the `_sysy_starttime(int)`/`_sysy_stoptime(int)` functions declared in `sylib.h`, then test source-line forwarding.
- Confirm cross-toolchain/QEMU availability or installability in Linux CI and pin versions.
- Decide exact CLI flags and default output path before public release; once documented, retain them as external contract.
- Verify LLVM/Clang differential tests only use common defined semantics, especially floating NaNs, integer overflow, and zero-length dimensions (the SysY text says nonnegative but ordinary arrays and ABI may not make zero extent meaningful).
