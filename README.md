# SysY 2022 compiler

A single C++23 `compiler` executable translates one SysY source file into **RISC-V RV64GC / LP64D GNU/Linux assembly**. It has a handwritten lexer/parser, semantic analysis, an internal SSA-capable IR, optimization levels `-O0` through `-O2`, source diagnostics, and optional SQLite observability. GitHub Actions is configured to build and test the host executable on Linux, macOS, and Windows; that matrix is not a claim that every runner result has already been observed. Generated assembly targets RISC-V GNU/Linux, not the host architecture.

## Build and run

Prerequisites: CMake 3.25+, Ninja, a C++23 compiler, SQLite 3 development headers/libraries, and Python 3 for the test harness. See [platform-specific setup](docs/build.md).

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
.cache/build/release/compiler input.sy -o output.s -O2
```

On Windows, the executable may be named `compiler.exe`. `debug` is the other shared preset. Build and test artifacts stay under `.cache/`; repository-local experiments belong in `.temp/`.

`compiler input.sy` defaults to `input.s`. `compiler - -o -` reads source from standard input and writes assembly to standard output. To link on a RISC-V GNU/Linux machine, or with a compatible cross-toolchain:

```sh
mkdir -p .temp
riscv64-linux-gnu-gcc -fcommon -c libs/libsysy/sylib.c -o .temp/sylib.o
riscv64-linux-gnu-gcc output.s .temp/sylib.o -o .temp/program
qemu-riscv64 -L /usr/riscv64-linux-gnu .temp/program
```

The exact QEMU sysroot path depends on the cross-toolchain installation. The bundled `libsysy_riscv.a` is **Newlib-linked** and cannot be mixed with a glibc GNU/Linux target; compiling `sylib.c` with the same GNU/Linux toolchain avoids that ABI mismatch. Linux CI is configured for target execution with a RISC-V cross-compiler and QEMU. macOS and Windows CI jobs are configured for host-side build and tests; they do not imply native target execution or an already-observed passing result.

## Interface and scope

- [Language, lexical regexes, grammar, and boundaries](docs/language.md)
- [CLI, diagnostic streams, and SQLite observability](docs/cli.md)
- [Build and cross-platform test details](docs/build.md)
- [Task language specification](docs/task/SysY2022语言定义-V1.md) and [binding grammar supplement](docs/task/SysY文法补充说明.md)

`--help`, diagnostics, and `--summary` use standard error; only explicitly requested assembly uses standard output. Automatic color is limited to capable terminals, with `--color=always|never|auto` available for control. `--db PATH` persists machine-readable runs, spans, events, metrics, and logs in SQLite; without it, compilation does not open a telemetry database. See the CLI reference for exit codes and trace propagation.

This repository implements the language documented in `docs/task`, with explicit boundaries such as no user function prototypes and no optional `putf` extension. It is not a general C compiler. In particular, treat benchmark and optimization claims as workload-dependent; the [optimization note](docs/architecture/optimization.md) records implemented passes and validation limits.
