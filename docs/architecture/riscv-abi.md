# RISC-V backend ABI decision

## Contract and evidence

The emitted target is RV64GC GNU/Linux with the LP64D ABI. The authority is the [RISC-V ELF psABI calling-convention specification](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc), especially the integer and hardware-floating-point calling convention sections. The SysY domain has signed `int32`, IEEE binary32 `float`, and internal 64-bit pointers; it does not require aggregates or variadic calls (`putf` is outside the required subset). All externally visible symbols use unmangled C linkage.

| Value | Passing/return rule | Machine representation |
|---|---|---|
| `i32` | Next `a0`–`a7`, else 8-byte stack slot; return `a0` | Sign-extended to XLEN, including after arithmetic. |
| pointer | Next `a0`–`a7`, else 8-byte stack slot; return `a0` | Full 64 bits. |
| named `f32` | Next `fa0`–`fa7`, else next available `a*`, else 8-byte stack slot; return `fa0` | `flw`/`fmv.s` NaN-box register values; integer fallback carries raw low 32 bits. |

The two register counters are independent. A float occupying `fa0` does **not** consume `a0`. Once the eight float registers are exhausted, another float follows the base integer calling convention and may consume `a0` even if this is the ninth source argument. Stack arguments are ordered by *stack allocation*, not original ordinal. The caller places the first overflow argument at `sp+0`; a callee finds it at its entry `sp+0`. Stack pointer stays 16-byte aligned at all times. `ra` is saved by every generated function, so recursive calls and nested runtime calls work. Generated code uses only caller-saved temporaries (`t0`, `t1`, `t5`, `ft0`, `ft1`) and need not save `s*`/`fs*`. LP64D does require preserving `fs0`–`fs11` if used in future register allocation.

## Frame and Phi strategy

Each SSA value has a fixed 8-byte stack home. The frame starts with a reserved outgoing-argument area, followed by value homes, aligned `alloca` objects, Phi staging homes, saved `ra`, and padding to a multiple of 16. This avoids dynamic stack adjustment around calls and makes stack argument offsets stable. A frame > 12-bit immediate range uses `li` plus register arithmetic for both prologue and accesses.

Phi values are resolved *on each predecessor edge*, including the two edges of a conditional branch. All source values are first copied into dedicated staging slots and only then copied to destination homes. Thus Phi assignment is parallel even for cyclic loop-carried dependencies; no CFG critical-edge split is required at assembly level. This is intentionally a correctness-oriented baseline. Value homes cause many memory operations, especially in loops, and should be reduced by a later bounded local register cache only after executable differential tests establish correctness. The API buffers the entire assembly before writing to the caller's stream, so a code-generation error cannot publish a partial assembly artifact.

## Numeric and object details

`i32` addition/subtraction/multiplication/division/remainder use RV64 `*w` instructions. Comparison operands are sign-extended on load; `slt` therefore has signed SysY semantics. Float comparisons use `feq.s`/`flt.s`/`fle.s`, and `!=` negates ordered equality so unordered NaNs compare unequal to zero as C-style truthiness requires. `fcvt.w.s ..., rtz` implements truncation toward zero. Float constants preserve exact input bits via `fmv.w.x` rather than decimal assembler conversion. Globals with all-zero words go to `.bss`, nonzero mutable globals to `.data`, constants to `.rodata`; array words retain row-major order.

## Verification gate

Compile backend C++ with `-Wall -Wextra -Werror`, generate assembly from manually constructed IR that exercises mixed and overflow arguments, recursion, globals, arrays, Phi cycles, and floats, then assemble/link/run with `riscv64-linux-gnu-gcc` and `qemu-riscv64`. The cross compiler is present in the host's WSL Ubuntu environment. A native `clang++ -fsyntax-only` check alone is insufficient evidence of ABI correctness. The direct ABI fixtures already passed: an emitted caller of a GCC C oracle with 9 floats plus 9 integers returned 77 as expected; a GCC C caller of an emitted callee with the same signature returned 26 as expected.

**Runtime archive compatibility finding:** the supplied `libs/libsysy/libsysy_riscv.a` cannot link with Ubuntu's glibc-targeting `riscv64-linux-gnu-gcc`: the linker reports undefined `_impure_ptr` references from `putfarray`, `before_main`, and `after_main`. That symbol is a newlib runtime detail, suggesting the archive targets a different C library. Recompiling the accompanying `sylib.c` with the glibc cross compiler into `.temp/sylib-glibc.o` linked and ran an end-to-end SysY program (global array, while loop, float function and `putfloat`) under QEMU, printing `0x1.ep+2` and returning 6. Release CI should rebuild the provided *source* for its target libc or use a matching newlib toolchain/archive, rather than hide this link incompatibility.

## Known boundary

This backend assumes verified IR. It does not implement varargs, aggregates, thread-local storage, PIC, or dynamic `alloca`; none are SysY-required. Call a verifier before emission. If the IR contract changes, especially Store operand order or global layout, update this document and an ABI fixture together rather than adding ad hoc codegen exceptions.
