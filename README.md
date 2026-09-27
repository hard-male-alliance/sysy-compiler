# SysY 2022 compiler

A single C++23 `compiler` executable translates one SysY source file into **RISC-V RV64GC / LP64D GNU/Linux assembly**. It has a handwritten lexer/parser, semantic analysis, an SSA-capable IR with scalar mem2reg, optimization levels `-O0` through `-O2`, source diagnostics, and optional SQLite observability. The language also supports compatible function prototypes for forward calls. GitHub Actions builds and tests the host executable on Linux, macOS, and Windows; generated assembly targets RISC-V GNU/Linux, not the host architecture.

## Build and run

Prerequisites: CMake 3.25+, Ninja, a C++23 compiler, SQLite 3 development headers/libraries, and Python 3 for the test harness. See [platform-specific setup](docs/build.md).

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
.cache/build/release/compiler input.sy -o output.s -O2
```

On Windows, the executable may be named `compiler.exe`. The shared presets are `debug`, `release`, and Linux-only `sanitizer`. Build and test artifacts stay under `.cache/`; repository-local experiments belong in `.temp/`.

`compiler input.sy` defaults to `input.s`. `compiler - -o -` reads source from standard input and writes assembly to standard output. For static target-program linking on a GNU/Linux RISC-V toolchain, build a matching runtime archive in an **isolated Linux/WSL build directory**:

```sh
cmake -S . -B .cache/build/wsl-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DSYSY_BUILD_RISCV_RUNTIME=ON
cmake --build .cache/build/wsl-runtime --parallel
mkdir -p .temp
.cache/build/wsl-runtime/compiler tests/conformance/basic.sy -o .temp/basic.s
riscv64-linux-gnu-gcc -static .temp/basic.s \
  .cache/build/wsl-runtime/runtime/riscv64-linux-gnu/libsysy.a -o .temp/basic.elf
qemu-riscv64 .temp/basic.elf
```

The bundled `libsysy_riscv.a` is **Newlib-linked** and cannot be mixed with a glibc GNU/Linux target. The opt-in CMake target builds `libsysy.a` from the supplied `sylib.c` using the same GNU/Linux RISC-V toolchain, without changing the compiler binary or the vendor archive. Linux CI executes target programs with this archive; macOS and Windows run host-side compiler tests. See the [runtime-linking decision](docs/architecture/runtime-linking.md) for ABI evidence and limitations.

## Interface and scope

- [Language, lexical regexes, grammar, and boundaries](docs/language.md)
- [CLI, diagnostic streams, and SQLite observability](docs/cli.md)
- [Build and cross-platform test details](docs/build.md)
- [Task language specification](docs/task/SysY2022语言定义-V1.md) and [binding grammar supplement](docs/task/SysY文法补充说明.md)

`--help`, diagnostics, and `--summary` use standard error; only explicitly requested assembly uses standard output. Automatic color is limited to capable terminals, with `--color=always|never|auto` available for control. `--db PATH` persists machine-readable runs, spans, events, metrics, and logs in SQLite; without it, compilation does not open a telemetry database. See the CLI reference for exit codes and trace propagation.

This repository implements the language documented in `docs/task`, plus self-contained function prototypes as a documented extension. `putf` remains optional and unimplemented. It is not a general C compiler. In particular, treat benchmark and optimization claims as workload-dependent; the [optimization note](docs/architecture/optimization.md) records implemented passes and validation limits.
