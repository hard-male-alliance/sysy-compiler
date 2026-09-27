# Semantic model and initializer contract

The semantic pass consumes the parser's `Program` and returns a `SemanticResult`. Code generation must stop if `errors` is nonempty. It does not mutate the AST. The public side tables in `include/compiler/semantic.hpp` key by AST-node address; therefore the `Program` must outlive the model and must not be copied or rebuilt between analysis and lowering. `SemanticModel` is deliberately move-only because copying would invalidate its stored `Symbol*` identities.

## Resolved identity and types

`symbols` is a deque so identities referenced by the other maps survive insertion. `references` resolves variable and call expressions, while `definitions`, `parameters`, and `functions` link declaration nodes to symbols. `Type::dimensions` is source-order, and an omitted leading array-parameter bound is represented by `-1`. An indexed array expression drops one leading bound per index; full indexing yields a scalar. Array actual/formal compatibility checks element type, rank, and all trailing extents; the leading extent is intentionally ignored.

The source-order top-level rule is enforced: a function can recurse but cannot call a later user-defined function without a prototype (the grammar supplies no prototype). Object lookup uses nested lexical scopes, while call syntax resolves the global function namespace independently: a local object named `f` does not hide a callable `f()`, as the SysY identifier note permits. Builtins are installed before source traversal. The runtime's `starttime()` and `stoptime()` are accepted as zero-argument builtins; lowering must map them to `_sysy_starttime(line)` and `_sysy_stoptime(line)`. `putf` is omitted because the task supplement makes it optional.

## Initializer representation

For any explicitly initialized declaration, `Symbol::initializer` contains one row-major `InitSlot` per **logical** scalar element. Globals without explicit initializers also receive zero-valued slots. Uninitialized locals have an empty vector: this is *not* a promise of zero initialization. A zero-extent array also has an empty vector even when explicitly initialized with `{}`; distinguish it by its dimensions, not vector emptiness. An implicit zero has `expression == nullptr` and a typed zero constant. An explicit expression points to the AST and carries a converted `constant` only when it was statically evaluable; runtime local initializers have `constant == nullopt`.

Nested braces align to the next complete subarray at the current rank; scalar entries advance one element. This gives `int a[3][2] = {1,2,{3,4},5};` the flattened values `[1,2,3,4,5,0]`. Excess values, non-scalar elements, and incompatible int-array float elements are diagnosed. The flattening limit of ten million elements prevents accidental analysis-time memory exhaustion; this is an implementation limit, not a language-standard bound.

## Explicit boundary decisions

The language document explicitly permits nonnegative dimensions. Zero-extent arrays therefore have **zero logical elements**; only `{}` is valid when an initializer is present. They require a minimum physical backing cell in lowering so their address can be represented without a zero-byte `Alloca` or zero-size global symbol; no element is accessible within bounds. Every nonzero dimension factor is constrained so the potential row-major stride stays within the implementation's ten-million-element bound even if another dimension is zero. Signed int constant folding refuses overflow, avoiding host C++ undefined behavior. Float-to-int compile-time conversion declines out-of-range values. A non-void function's fallthrough is not diagnosed because the task specifies an undefined result there, while a mismatched explicit `return` is diagnosed.

This document describes the pass contract and design decisions; focused parser-to-semantics probes were compiled and run locally with GCC C++23. It does not claim target-level execution correctness.

## External cross-check

The [WG14 C draft N1516 §6.7.9](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1516.pdf) describes brace-enclosed subaggregates and implicit initialization of omitted elements. This corroborates the flatten-and-zero-fill model, while the repository's SysY task document remains authoritative for the particular mixed-brace examples. LLVM's [IR language reference](https://llvm.org/docs/LangRef.html) explicitly distinguishes signed-overflow and division-by-zero behavior; this reinforces the decision not to invent a folded int32 result when a constant arithmetic operation is outside the representable or defined domain. Neither source validates the future backend's target behavior; differential target tests remain necessary.
