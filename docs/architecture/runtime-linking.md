# GNU/Linux RISC-V SysY runtime linkage

## Decision and scope

The supplied `libs/libsysy/libsysy_riscv.a` is **not** a usable runtime archive for
`riscv64-linux-gnu-gcc` on GNU/Linux. Keep the vendor artifact unmodified for
course/toolchain compatibility, but compile its supplied `sylib.c` with the same
GNU/Linux RISC-V cross toolchain used to link generated programs. The build-tree
archive is an explicit, opt-in CMake target rather than another shipped binary.
This solves the Linux/glibc E2E path; it does not assert that the vendor archive
is invalid for the Newlib environment for which it was apparently built.

## Observed evidence and causal mechanism

On Ubuntu 24.04 in WSL, `riscv64-linux-gnu-gcc -dumpmachine` reports
`riscv64-linux-gnu` (GCC 13.3.0, binutils 2.42). `riscv64-linux-gnu-nm -u
libs/libsysy/libsysy_riscv.a` lists `_impure_ptr`; the vendor archive also
references `fprintf`, `gettimeofday`, `printf`, `putchar`, `scanf`, and
`vfprintf`. Linking `tests/conformance/basic.sy`'s generated assembly against
that archive using `riscv64-linux-gnu-gcc -static` fails in `putfarray`,
`before_main`, and `after_main` with undefined reference to `_impure_ptr`.
This is a C-library ABI mismatch, not a missing SysY symbol or a backend calling
convention defect. Newlib documents `_impure_ptr` as its global pointer to
thread-specific reentrancy state; glibc does not provide that Newlib contract.
A fake `_impure_ptr` symbol would make the linker quiet without making the
library semantically compatible.

The original `sylib.h` contains tentative global definitions. GCC 10 and later
default to `-fno-common`; `-fcommon` when compiling this *runtime source only*
retains the original common-symbol treatment if the header is included in other
translation units. The present E2E link uses only `sylib.c`, so this flag is not
intrinsically necessary for that one-object link. It is independent of the
Newlib/glibc issue. We keep the upstream source unchanged, including its
unchecked `scanf` calls (which emit warnings with Ubuntu's fortified headers).
Changing input-failure behavior requires a separate contract decision.

## Build contract

Use `-DSYSY_BUILD_RISCV_RUNTIME=ON` and optionally set `SYSY_RISCV_CC` and
`SYSY_RISCV_AR` to tools from the same GNU/Linux RISC-V toolchain. Configure
checks the compiler's `-dumpmachine` result matches `riscv64.*linux-gnu`.
The `sysy_runtime_riscv` target, included in the default build only when enabled,
produces `${binaryDir}/runtime/riscv64-linux-gnu/libsysy.a` by compiling
`sylib.c` with `-std=c11 -O2 -fcommon -march=rv64gc -mabi=lp64d`, then invoking
the matching `ar rcs`. The source and header are tracked as build dependencies.
The host compiler executable and CLI do not change. macOS and Windows default
builds do not discover or require cross tools. The Linux CI build opts in and
CTest's conformance test receives this archive via `--runtime-archive`.

A final program must be linked by a compatible toolchain and ABI, e.g.:

```sh
cmake -S . -B .cache/build/wsl-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSYSY_BUILD_RISCV_RUNTIME=ON
cmake --build .cache/build/wsl-runtime --parallel
mkdir -p .temp/runtime-link
.cache/build/wsl-runtime/compiler tests/conformance/basic.sy -o .temp/runtime-link/basic.s
riscv64-linux-gnu-gcc -static -march=rv64gc -mabi=lp64d \
  .temp/runtime-link/basic.s \
  .cache/build/wsl-runtime/runtime/riscv64-linux-gnu/libsysy.a \
  -o .temp/runtime-link/basic.elf
qemu-riscv64 .temp/runtime-link/basic.elf
```

The static archive must occur **after** the generated assembly/object on the
link line, so its members are pulled for unresolved SysY references. Reconfigure
and rebuild in a fresh build directory after switching cross toolchain or ABI.
Do not link Newlib and glibc objects together. For native Newlib targets, the
bundled archive can be investigated separately with a matching Newlib linker;
this project does not claim to support that execution environment.

## Local verification, 2026-09-27

- Configured a separate `.cache/build/wsl-runtime-link` tree with
  `SYSY_BUILD_RISCV_RUNTIME=ON`; built `sysy_runtime_riscv` successfully.
- `riscv64-linux-gnu-nm -u` on the resulting archive has no `_impure_ptr`;
  its remaining undefined symbols are expected glibc/libgcc functions such as
  `__isoc99_scanf`, `gettimeofday`, and `memset`.
- Linked generated `basic.s` with the build-tree archive using the command
  above (with the actual local build paths). `qemu-riscv64` printed `42` and
  exited successfully; runtime timing information went to stderr.
- Full updated CTest/CI conformance remains separately tracked in
  `docs/testing/conformance.md`.

## Primary references

- [Newlib C library documentation, reentrancy and `_impure_ptr`](https://sourceware.org/newlib/libc.html)
- [GCC `-fcommon` and `-fno-common` variable attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html)
- [CMake `add_custom_command` generated-output/dependency rules](https://cmake.org/cmake/help/latest/command/add_custom_command.html)
