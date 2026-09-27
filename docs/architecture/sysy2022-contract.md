# SysY 2022 compiler contract and conformance checklist

## Scope and authority

This note records the repository-local task contract, not an independently invented SysY dialect. Sources inspected on 2026-09-27: `docs/task/上机大作业总体要求.md` (course deliverable), `docs/task/SysY2022语言定义-V1.md` (language), `docs/task/SysY文法补充说明.md` (explicit corrections), `docs/task/SysY2022运行时库-V1.md` (runtime prose), and `libs/libsysy/{sylib.h,sylib.c,README.md}` (bundled ABI/behavior). The supplement prevails where it explicitly corrects the language document; actual runtime source prevails when linking the bundled binaries. No test-suite or grader CLI protocol is supplied by these files.

## Build and external interface

| Contract | Evidence | Verification implication |
| --- | --- | --- |
| One `.sy` source file per program, exactly one `int main()` with no parameters. | Language §1; §3 CompUnit 1 | Reject missing/duplicate/wrong-signature `main`; maintain source locations. |
| Course target is Linux ARM **or** RISC-V assembly, linked by GCC/Clang to an executable, with static SysY runtime for formal evaluation. | General requirements §3; runtime §1 | A host x86 executable or LLVM IR alone is not the requested target artifact. Choose and document a target ABI and cross-toolchain. |
| Task expects a description of the implemented lexical regexes and CFG, plus compiler sources. | General requirements §3 | Keep a language/grammar deliverable synchronized with implementation. |
| No invocation spelling, assembly filename, optimization flag semantics, exit-code convention, or diagnostic format is specified. | All task documents | Define stable compiler CLI explicitly; avoid claiming a grader-specific CLI. stdout must remain available for requested compiler output and compiled-program output; stderr is appropriate for diagnostics. |

## Lexical and syntactic checklist

| Area | Required behavior and discriminating examples | Source |
| --- | --- | --- |
| Names and comments | ASCII letters/underscore followed by ASCII letters/digits/underscore; `//` to newline, `/*` through first `*/`. No nested comments promised. | Language terminal features §§1–2 |
| Numerals | Decimal, leading-zero octal, and `0x`/`0X` hexadecimal integers; decimal and hexadecimal C-style floating literals, *without suffixes*. Keep enough magnitude to handle the special `-2147483648` interpretation before narrowing to 32-bit signed. | Language terminal features §3; supplement 2, 4 |
| Types | 32-bit signed `int`, binary32 `float`, `void` return only; scalar and row-major multidimensional arrays of `int`/`float`. Implicit int↔float conversions; no explicit casts. | Language §1 and §3 implicit conversion |
| Translation unit | Global variable/constant declarations and function definitions; top-level declaration/definition scope begins at textual position and extends to EOF. A call to a later function is not justified by the written prior-definition rule unless a separate declaration extension is specified. | Language §1, §3 CompUnit 2–3 and Exp/Cond 2–3 |
| Statements | Assignment, empty/ordinary expression statement, block, `if`/`else` (nearest unmatched `if`), `while`, `break`, `continue`, and `return`; no `for`, `++` operator, or assignment expression in the given grammar. `++a` lexes/parses as unary `+` twice. | Language EBNF and §3 Stmt; supplement 5 |
| Expressions | C-like precedence/associativity for unary, `* / %`, `+ -`, relations, equality, `&&`, `||`; `&&`/`||` must short-circuit and comparison/logical results are 0/1. Supplement allows `!` inside *any* expression (`return a + !a;`), overriding base prose that restricts `Exp` to `AddExp`. Parser grammar must therefore admit conditions/logical/relations wherever `Exp` occurs. | Language §1, EBNF, §3 Exp/Cond; supplement 3 |
| Calls | Typed scalar and array actual parameters, arity/type agreement, value passing for scalars and base-address passing for arrays. `void` calls allowed as statements; return-value use needs type checking. | Language §1, §3 FuncFParam and Exp/Cond |

## Semantic and code-generation checklist

| Area | Invariant or test oracle | Source |
| --- | --- | --- |
| Names/scopes | Reject duplicate identifiers at top level and overlapping same-scope locals; nested blocks can shadow outer declarations. Variables must be defined before use. A local variable may share a function name according to the lexical-name note, though the top-level no-duplicate rule limits collisions there. | Language §3 CompUnit, Block, Exp/Cond; terminal features §1 |
| Globals and locals | Absent initializer: global scalar/array elements zero; local value indeterminate. Global initializers must be compile-time constants. Do not silently zero-initialize uninitialized locals as a semantic promise. | Language §3 VarDef and Initial values |
| Constants and dimensions | `const` needs initializer; constant expressions may reference previously defined constants, including in global array dimensions (an intentional departure from C). Every defined array dimension explicit and compile-time nonnegative integer; shape, element type, and excess initializers must be checked. The document says nonnegative, not strictly positive: zero-sized dimensions remain an ambiguity. | Language §3 ConstDef, VarDef; supplement 1, 6 |
| Array initialization | Braces can mix flattened and nested subaggregates; each nested brace aligns with a complete remaining subarray, omitted elements zero-fill. E.g. `int a[3][2] = {1,2,{3,4},5};` gives `{{1,2},{3,4},{5,0}}`. Element type/range and initializer count constraints apply. Variable local initializers can contain runtime expressions; const/global values cannot. | Language §3 ConstDef 4–5, VarDef 3, Initial values |
| Array addressing | Row-major layout, zero-based indexing. Full subscripting produces scalar lvalue; partial subscripting can produce an address for an array argument (`a[1]` from `int a[4][3]` to `int[]`). For an array formal, only first dimension omitted; trailing dimensions are `ConstExp`, not the erroneous `Exp` in main EBNF. | Language §1 and §3 FuncFParam, LVal; supplement 1 |
| Control flow | `break`/`continue` target innermost `while`; `return` value must agree with `int`/`float` return, and `void` permits only bare `return`. A value-returning function can fall through, but its result is *undefined* according to the document: warning is reasonable, invented value is not. | Language §3 FuncDef; grammar Stmt |
| Numeric conversion | Float→int truncates fractional part; out-of-range is undefined. Int→float rounds to binary32 as needed (despite prose saying value “remains unchanged”, which cannot hold for all 32-bit ints). Boolean context uses nonzero true. Avoid relying on host C/C++ signed-overflow behavior in constant folding. | Language §1 and §3 implicit conversion; IEEE binary32 consequence is an inference |
| Optimization | Coursework requests complete scalar `mem2reg`, unreachable-block and dead-statement elimination; two additional passes from SCCP, scalar LICM, scalar CSE, inlining, or aggressive DCE/dead-loop elimination. Preserve side effects and short-circuit semantics through every pass. | General requirements §2 advanced optimization |

## Runtime ABI: declarations and discrepancies to test

The bundled header/source expose `getint`, `getch`, `getfloat`, `getarray(int*)`, `getfarray(float*)`, `putint`, `putch`, `putfloat`, `putarray(int,int*)`, `putfarray(int,float*)`, and variadic `putf(char*,...)`. Source programs **do not include** `sylib.h`; these must be compiler-known. Although the supplement says `putf` is not required, supporting it involves a string-literal exception to the core grammar and target variadic ABI. Runtime arrays are unchecked, so the compiler cannot delegate bounds safety to the library.

`starttime()` and `stoptime()` are C header macros, not exported zero-argument symbols. They expand to `_sysy_starttime(__LINE__)` and `_sysy_stoptime(__LINE__)`, respectively. A compiler must lower intrinsic calls to those symbols with the call-site source line number. The source implementation writes timer records and `TOTAL` to **stderr** at process teardown, unlike ordinary `put*` output on stdout. This is *compiled program* behavior, distinct from compiler diagnostics/telemetry. The documented timer example uses `Timer#...` while `sylib.c` actually prints `Timer@...`; test against the linked library, not the prose formatting.

The runtime prose shows `putfloat(10.0)` as `10.000000`, but bundled `sylib.c` uses `printf("%a", a)` for `putfloat` and `putfarray`, hence emits hexadecimal floating syntax. It likewise uses `scanf("%a", &n)` for `getfloat`/`getfarray`. `putarray`/`putfarray` append a newline; `putint`/`putch`/`putfloat` do not. The header declares `putf` variadic, yet the supplement explicitly excludes it from required functionality. The README contains stale `./lib` link paths; actual archives are under `libs/libsysy/`, named `libsysy_{x86,riscv,aarch}.a`.

## Ambiguities and decisions needed

1. **Target choice and host portability:** course accepts ARM or RISC-V; cross-platform CI for the compiler host need not imply native target execution on every host. Choose one primary target and obtain an executable/QEMU oracle in Linux CI. The bundled AArch64 archive is named `aarch`, not `aarch64`.
2. **Grammar contradiction:** formal `Exp -> AddExp` conflicts with supplement's any-position boolean/relational expressions; implement the supplement and document the revised CFG (e.g. `Exp -> LOrExp`, avoiding a parallel ad hoc condition parser).
3. **“Tensor type”:** the course's advanced checklist mentions it, but the language definition gives only multidimensional arrays and no independent tensor syntax or semantics. Treat arrays as the only concretely specified tensor-like construct until clarified.
4. **“Function declarations”:** course summary names them, yet language EBNF provides only `FuncDef`, and the prior-definition rule suggests no prototype or forward call. A prototype extension might be useful but cannot be claimed mandatory from the detailed language specification. **Implementation update (2026-09-27):** the compiler now accepts preceding, compatible same-unit function prototypes and requires their eventual definitions; this is an explicit extension, not a reinterpretation of the original task evidence. See [current architecture](compiler-architecture.md).
5. **C-reference gaps:** division by zero, signed overflow, overflow in constant expressions, non-finite float literals, zero-size arrays, `%` applied to float, uninitialized reads, array bounds, and exact string-literal escapes are not fully specified here. Follow applicable C semantics where the supplement directs, but explicitly document accepted/rejected cases and avoid optimizer assumptions on undefined inputs.
6. **Runtime output oracle:** prose and actual bundled library disagree on float and timer formatting. E2E tests should compare to the actual archive used by the chosen target. Record library revision/hash in CI if exact output is asserted.
7. **Missing linked diagrams:** this checkout has no `docs/task/assets/` although the language document references three images, so some array examples and identifier illustration cannot be inspected locally. Textual rules remain available.

## Minimum discriminating E2E cases

- Lexer/parser: octal `010`, hex `0X10`, hex float, `-2147483648`, `++a`, multiline comments, `return a + !a`, dangling `else`.
- Scope/diagnostics: nested shadowing, duplicate local/global, use before definition, bad `main`, wrong call arity/type, assignment to `const`, `break` outside loop.
- Arrays: const-derived dimensions, 3-D row-major indexing, partial subarray argument, flattened/mixed-brace zero-fill, excess and incompatible initializers, global zero-fill versus uninitialized local.
- Control/ABI: short-circuit suppresses RHS `putint`; recursion and nested loops; float↔int calls/returns; runtime array I/O; start/stop line numbers and separate stdout/stderr.
- Optimization differential: compare unoptimized and optimized executions on side-effecting short circuits, aliasing through array parameters, loops with `break`/`continue`, and undefined-input exclusion.

## Evidence limits

This is a source-document/source-code inspection, not a completed compile-and-run validation. Library archives were listed but not executed or binary-inspected. The note deliberately distinguishes written contract, source-level runtime observations, and proposed test/CLI decisions. The task documents provide no official test cases or reference compiler invocation in this checkout.
