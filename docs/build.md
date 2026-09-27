# Building and testing

The project produces one executable, `compiler`. It requires a C++23 compiler,
CMake 3.25 or newer, Ninja, Python 3 for conformance tests, and SQLite 3 development
headers and libraries. There is no LLVM dependency. SQLite is discovered through
CMake's `FindSQLite3` module and linked through its imported target; the project
accepts both the legacy `SQLite::SQLite3` and new `SQLite3::SQLite3` target names.

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Use `release` instead of `debug` for an optimized build. The binary and all build
artifacts are under `.cache/build/<preset>/`; local experiments belong in `.temp/`.
These paths and `CMakeUserPresets.json` are ignored by Git. Override machine-specific
settings in `CMakeUserPresets.json` rather than editing shared presets.

On Linux with GCC or Clang, a separate opt-in build instruments the compiler and
telemetry unit test with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer
(UBSan), without changing Debug or Release:

```sh
cmake --preset sanitizer
cmake --build --preset sanitizer --parallel
ctest --preset sanitizer
```

The sanitizer preset is unavailable on non-Linux hosts. It instruments host C++
targets, **not** generated RISC-V binaries or the system SQLite library. Failures
in a sanitized host process should be investigated independently from target-code
execution failures. GitHub Actions runs this preset as an additional Linux job.

Local verification on 2026-09-27 used Ubuntu 24.04 under WSL, GCC 13.3.0,
CMake 3.28.3, SQLite 3.45.1, and the installed RISC-V GCC/QEMU tools. From the
repository root, `cmake --preset sanitizer && cmake --build --preset sanitizer
--parallel 4 && ctest --preset sanitizer` configured and built both targets;
CTest passed `telemetry_unit` (0.18 s) and `sysy_conformance` (59.26 s), 2/2
tests, 0 failures. The conformance test executed generated RISC-V programs via
QEMU because both target tools were present. Hosted validation is recorded below.

The first hosted run, [GitHub Actions run 36312669966](https://github.com/hard-male-alliance/sysy-compiler/actions/runs/36312669966),
completed successfully on 2026-09-27: Ubuntu 24.04 Debug/Release, macOS 15
Debug/Release, Windows MinGW Debug/Release, and Linux ASan+UBSan all passed their
configure, build, and test steps (7/7 jobs). The run emitted only a deprecation
annotation for `actions/checkout@v4`'s Node.js 20 runtime; the workflow now uses
`actions/checkout@v5` (Node.js 24). This action update still needs a subsequent
hosted run to validate it; it does not change the project build itself.

On macOS, Homebrew's SQLite may be keg-only. In that case configure with
`-DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"`. On Windows, MSYS2 MINGW64 with the
`mingw-w64-x86_64-{gcc,cmake,ninja,sqlite3,python}` packages is the tested path.
The GitHub Actions workflow exercises Debug and Release on Linux, macOS, and
Windows and runs the same CTest suite on each platform. Linux additionally
installs the RISC-V cross compiler and QEMU user emulator so the conformance
suite can link and execute generated programs rather than only inspect assembly.
The harness builds the supplied `libs/libsysy/sylib.c` with that same glibc-based
cross compiler using `-fcommon`, then links its object with each generated program.
It deliberately does **not** link `libsysy_riscv.a`: that supplied archive contains
Newlib references such as `_impure_ptr` and is incompatible with Linux glibc.
This is a runtime-library ABI distinction, not an alternate compiler output format.

Conformance tests are registered with CTest when
`tests/sysy_conformance.py` is present. The script receives `--compiler` with the
absolute path to the built executable. Tests and experimental data must stay in
this repository rather than external temporary directories.

The `telemetry_unit` CTest target builds `tests/telemetry_test.cpp` with the same
telemetry implementation and SQLite/thread dependencies as the compiler. It runs
from the repository root so its database is written to `.temp/`.

## Rationale and constraints

Source files under `src/` are assembled into a single target. The recursive source
glob uses `CONFIGURE_DEPENDS` so adding a module retriggers CMake configuration,
which matters while compiler modules are developed concurrently. Public and
cross-module headers live under `include/`. A future split into internal libraries
must not change the installed `compiler` executable or its command-line interface.

The CI configuration intentionally installs SQLite rather than assuming a runner
image happens to contain it. The Windows job uses one MSYS2 toolchain end-to-end;
mixing MSVC objects with MinGW SQLite libraries would create an avoidable ABI risk.
The Linux E2E job likewise keeps generated code and its source-built runtime under
one RISC-V GNU/Linux toolchain, rather than mixing a Newlib archive with glibc.

References: [CMake presets schema](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html),
[FindSQLite3 imported target](https://cmake.org/cmake/help/v3.25/module/FindSQLite3.html),
[GitHub-hosted runner images](https://github.com/actions/runner-images).
