# IR lowering contract and verification notes

## Representation and ownership

`include/compiler/ir.hpp` is the shared typed middle-IR contract. A `ModuleIR` owns globals and functions by value. `FunctionIR` owns its blocks and value-type table; `ValueId` and `BlockId` are stable indices, not pointers. Instructions define SSA results or memory side effects, and each block has a distinct terminator. `Phi` instructions use predecessor/value pairs. All `PtrAdd` offsets are bytes; globals and aggregate initializers store row-major 32-bit words, with float words preserving IEEE binary32 bits.

The AST and `SemanticModel` must outlive `lower()` only. Semantic model pointers are identity keys into its stable symbol deque; the IR owns names and immediate data and does not borrow AST nodes. The driver must reject semantic errors before lowering and lowering errors before optimization/emission. `verify_ir()` checks the IR before and after optimization.

## Lowering decisions

- Scalar mutable locals and parameters use entry-block `Alloca` plus explicit `Load`/`Store`; array formals are already pointer values. This keeps aliasing visible and allows the optimizer to promote only safe scalar allocas.
- A nested array subscript contributes `index * product(trailing extents) * 4` bytes. The first omitted bound of an array parameter never enters a trailing stride.
- Zero-extent arrays retain logical element count zero, but lowering reserves one 32-bit backing word so a pointer to the object can be represented. Element access remains out-of-bounds and has no defined result; the backing word is not a logical element.
- Logical `&&`, `||`, and `!` lower through CFG edges, not eager arithmetic. A logical value is materialized with a join-block `Phi(1,0)`, preserving short-circuit side effects. Condition truth is `value != 0`, including float values and NaN.
- Local initializer slots come from semantic brace-aware flattening. Explicit expressions are evaluated in slot order; implicit slots emit zero stores. Globals use only the constant values supplied by semantic analysis.
- Source-level `starttime()`/`stoptime()` lower to `_sysy_starttime(line)`/`_sysy_stoptime(line)`; the argument is the call expression's one-based source line.
- SysY allows falling off a value-returning function with an undefined result. Lowering chooses a deterministic zero result only for that undefined case; users must not depend on it.
- The exact `-2147483648` spelling is a semantic special case: its positive literal child is not itself an i32. Lowering emits the folded `INT32_MIN` rather than trying to lower that child.

## Local verification evidence (2026-09-27)

On Windows/MinGW GCC 16.1, `g++ -std=c++23 -Wall -Wextra -Wpedantic -Iinclude -fsyntax-only src/lower.cpp` passed. A temporary harness under `.temp/` parsed, analyzed, lowered, and ran `verify_ir()` on all 14 accepted `tests/conformance/*.sy` fixtures, plus probes for `-2147483648`, nested logical conditions, multidimensional array arguments, floating conversion, and loop `break`/`continue`; all passed. `cmake --build --preset debug` and `ctest --preset debug` passed after the independent diagnostics linker fix. These are structural and host-CLI checks, not proof of RISC-V runtime correctness; QEMU end-to-end validation is tracked separately.
