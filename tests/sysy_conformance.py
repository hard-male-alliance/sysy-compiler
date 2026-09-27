"""Run black-box SysY compiler and optional RISC-V execution conformance checks.

Usage: python tests/sysy_conformance.py --compiler path/to/compiler [--execute auto|on|off]
用法：传入编译器路径；仅在可用交叉编译器和 QEMU 时自动运行目标程序。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "tests" / "conformance"
WORK = ROOT / ".temp" / "conformance"

# Expected stdout follows the provided SysY runtime library and language semantics.
# 预期输出依据项目内 SysY 语言定义与运行时库，不由编译器输出反推。
VALID = {
    "basic": ("42\n", ""),
    "integers": ("30\n", ""),
    "scopes_loops": ("1 2 4 5 6 18\n", ""),
    "short_circuit": ("62\n", ""),
    "functions": ("120\n", ""),
    "arrays": ("13\n", ""),
    "global_array": ("3\n", ""),
    "float": ("1\n", ""),
    "float_array": ("1\n", ""),
    "input": ("42\n", "41\n"),
    "comment_unary": ("5\n", ""),
    "number_edges": ("1\n", ""),
    "many_args": ("55\n", ""),
    "dangling_else": ("2\n", ""),
    "timing": ("6\n", ""),
    "mixed_args": ("1\n", ""),
    "same_name_call": ("", ""),
    "zero_extent": ("1\n", ""),
    "nested_phi": ("243\n", ""),
    "runtime_initializer": ("16\n", "3\n"),
    "recursive_array": ("10\n", ""),
    "negative_zero": ("1\n", ""),
    "float_io": ("0x1.8p+0\n", "0x1.8p0\n"),
    "proto_forward": ("42\n", ""),
    "proto_mutual": ("2\n", ""),
    "proto_repeated": ("42\n", ""),
    "proto_array": ("6\n", ""),
    "proto_void_parameter": ("42\n", ""),
    "mem2reg_defined_path": ("7\n", ""),
}

# SysY permits a local object to share a function name; the function call must
# still resolve to the function. This valid fixture intentionally returns 8.
# SysY 允许局部对象与函数同名；函数调用仍须解析为函数。
EXPECTED_EXIT = {"same_name_call": 8}

INVALID = (
    "bad_token",
    "bad_comment",
    "bad_syntax",
    "bad_duplicate",
    "bad_undefined",
    "bad_const_write",
    "bad_break",
    "bad_octal",
    "bad_proto_mismatch",
    "bad_proto_duplicate_def",
    "bad_proto_missing_main",
    "bad_proto_undefined",
    "bad_proto_array_shape",
)


def run(args: list[str], *, stdin: str = "") -> subprocess.CompletedProcess[str]:
    """Run a bounded child process and capture both channels independently.

    在固定超时内运行子进程，分别捕获标准输出和标准错误。
    """
    return subprocess.run(
        args, input=stdin, capture_output=True, text=True, timeout=30, check=False
    )


def check(condition: bool, label: str, detail: str = "") -> bool:
    """Report an observable assertion without hiding a failed case.

    逐项报告可观测断言，避免用单个测试数量掩盖失败。
    """
    print(f"{'PASS' if condition else 'FAIL'} {label}" + (f": {detail}" if detail else ""))
    return condition


def compile_source(compiler: Path, name: str, extra: list[str] | None = None) -> tuple[subprocess.CompletedProcess[str], Path]:
    """Compile one fixture using the documented single-file CLI contract.

    使用约定的单文件命令行接口编译一个样例。
    """
    output = WORK / f"{name}.s"
    output.unlink(missing_ok=True)
    command = [str(compiler), str(SOURCES / f"{name}.sy"), "-o", str(output)]
    if extra:
        command.extend(extra)
    return run(command), output


def main() -> int:
    """Check accepted/rejected programs, streams, metadata, and optional execution.

    检查有效/无效程序、输出流、元数据与可选的目标程序运行。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--execute", choices=("auto", "on", "off"), default="auto")
    parser.add_argument("--opt-levels", default="0,1,2", help="comma-separated compiler optimization levels")
    parser.add_argument("--runtime-archive", type=Path, help="matching RISC-V GNU/Linux static runtime archive")
    args = parser.parse_args()
    levels = args.opt_levels.split(",")
    if not levels or any(level not in ("0", "1", "2") for level in levels):
        parser.error("--opt-levels must contain only 0, 1, or 2")
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        print(f"FAIL compiler executable not found: {compiler}")
        return 2
    WORK.mkdir(parents=True, exist_ok=True)

    gcc = shutil.which("riscv64-linux-gnu-gcc")
    qemu = shutil.which("qemu-riscv64")
    runtime_archive = args.runtime_archive.resolve() if args.runtime_archive else None
    if runtime_archive is not None and not runtime_archive.is_file():
        print(f"FAIL requested runtime archive not found: {runtime_archive}")
        return 2
    can_execute = bool(gcc and qemu and runtime_archive)
    if args.execute == "on" and not can_execute:
        print("FAIL execution requested but RISC-V GCC, QEMU, or matching runtime archive is missing")
        return 2
    execute = args.execute == "on" or (args.execute == "auto" and can_execute)
    if not execute:
        print("SKIP target execution: RISC-V GCC/QEMU/matching archive unavailable or disabled; compile-only evidence follows")

    failures = 0
    for level in levels:
        for name, (expected, stdin) in VALID.items():
            label = f"{name} -O{level}"
            result, output = compile_source(compiler, name, [f"-O{level}"])
            accepted = result.returncode == 0 and output.is_file() and output.stat().st_size > 0
            failures += not check(accepted, f"accept {label}", result.stderr.strip() if not accepted else "")
            failures += not check(result.stdout == "", f"stdout reserved {label}", repr(result.stdout[:160]) if result.stdout else "")
            if accepted and name == "timing":
                assembly = output.read_text(errors="replace")
                failures += not check(
                    "_sysy_starttime" in assembly and "_sysy_stoptime" in assembly,
                    f"timing builtin calls {label}",
                )
            if not accepted or not execute:
                continue
            executable = WORK / f"{name}-O{level}.elf"
            link = run([gcc, "-static", str(output), str(runtime_archive), "-o", str(executable)])
            linked = link.returncode == 0
            failures += not check(linked, f"link {label}", link.stderr.strip() if not linked else "")
            if not linked:
                continue
            actual = run([qemu, str(executable)], stdin=stdin)
            failures += not check(
                actual.returncode == EXPECTED_EXIT.get(name, 0) and actual.stdout == expected,
                f"execute {label}",
                f"exit={actual.returncode}, expected_exit={EXPECTED_EXIT.get(name, 0)}, expected_stdout={expected!r}, got={actual.stdout!r}, stderr={actual.stderr[:160]!r}",
            )
            if name == "timing":
                failures += not check(
                    bool(re.search(r"Timer@0*2-0*4:", actual.stderr)) and "TOTAL:" in actual.stderr,
                    f"runtime timer stderr {label}",
                    repr(actual.stderr[:160]),
                )

    for name in INVALID:
        result, output = compile_source(compiler, name, ["--color=never"])
        failures += not check(result.returncode != 0, f"reject {name}")
        failures += not check(bool(result.stderr.strip()) and result.stdout == "", f"stderr diagnostic {name}")
        failures += not check("\x1b[" not in result.stderr, f"color never {name}")
        failures += not check(not output.exists(), f"no failed output {name}")
        # Rich source locations should include line and column, but different
        # spelling is acceptable (e.g. path:1:14 or line 1, column 14).
        # 丰富诊断应指出行列；容许常见的不同拼写。
        location = bool(re.search(r":\d+:\d+|line\s+\d+.{0,16}column\s+\d+", result.stderr, re.I))
        failures += not check(location, f"source location {name}")
        failures += not check(" | " in result.stderr and "^" in result.stderr, f"source excerpt and caret {name}")

    result, _ = compile_source(compiler, "bad_token", ["--color=always"])
    failures += not check("\x1b[" in result.stderr and result.stdout == "", "forced color on stderr")

    unsafe_arg = "--unexpected\x1b[31m"
    escaped = run([str(compiler), unsafe_arg])
    failures += not check(
        escaped.returncode == 2 and escaped.stdout == "" and "\x1b" not in escaped.stderr,
        "terminal control bytes escaped from CLI argument",
        repr(escaped.stderr[:160]),
    )

    db = WORK / "run.sqlite"
    db.unlink(missing_ok=True)
    result, _ = compile_source(compiler, "basic", ["--color=never", "--db", str(db), "--summary"])
    failures += not check(result.returncode == 0, "SQLite/summary invocation", result.stderr if result.returncode else "")
    failures += not check(result.stdout == "" and bool(result.stderr.strip()), "summary on stderr only")
    if db.exists():
        try:
            with sqlite3.connect(db) as conn:
                tables = [row[0] for row in conn.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")]
                count = sum(conn.execute(f'SELECT COUNT(*) FROM "{table}"').fetchone()[0] for table in tables)
            failures += not check(bool(tables) and count > 0, "SQLite has run records", f"tables={tables}, rows={count}")
        except sqlite3.DatabaseError as error:
            failures += not check(False, "SQLite valid database", str(error))
    else:
        failures += not check(False, "SQLite file created")

    preserved = WORK / "preserved.s"
    sentinel = "# pre-existing userspace output must survive a failed compile\n"
    preserved.write_text(sentinel)
    result = run([str(compiler), str(SOURCES / "bad_token.sy"), "-o", str(preserved), "--color=never"])
    failures += not check(result.returncode != 0 and bool(result.stderr.strip()), "failed compile reports error")
    failures += not check(preserved.read_text() == sentinel, "failed compile preserves existing output")

    published = WORK / "published.s"
    published.write_text(sentinel)
    result = run([str(compiler), str(SOURCES / "basic.sy"), "-o", str(published), "--color=never"])
    replacement = published.read_text()
    failures += not check(
        result.returncode == 0 and result.stdout == "" and replacement != sentinel
        and ".text" in replacement and "main:" in replacement,
        "successful compile replaces pre-existing assembly output",
        f"exit={result.returncode}, stderr={result.stderr[:160]!r}",
    )

    streamed = run([str(compiler), str(SOURCES / "basic.sy"), "-o", "-", "--summary", "--color=never"])
    failures += not check(
        streamed.returncode == 0 and ".text" in streamed.stdout and "main:" in streamed.stdout
        and not re.search(r"summary", streamed.stdout, re.I)
        and bool(streamed.stderr.strip()) and ".globl main" not in streamed.stderr,
        "stdout assembly and stderr summary stay separated",
        f"exit={streamed.returncode}, stdout={streamed.stdout[:80]!r}, stderr={streamed.stderr[:120]!r}",
    )

    trace_id = "0123456789abcdef" * 2
    parent_id = "fedcba9876543210"
    trace_db = WORK / "remote-trace.sqlite"
    trace_db.unlink(missing_ok=True)
    result, _ = compile_source(
        compiler, "basic", ["--db", str(trace_db), "--traceparent", f"00-{trace_id}-{parent_id}-01"]
    )
    failures += not check(result.returncode == 0, "remote trace invocation", result.stderr if result.returncode else "")
    if trace_db.exists():
        try:
            with sqlite3.connect(trace_db) as conn:
                runs = conn.execute("SELECT trace_id, remote_parent_span_id, root_span_id FROM run").fetchall()
                roots = conn.execute("SELECT trace_id, parent_span_id FROM span WHERE name = 'compiler.run'").fetchall()
            linked = (
                len(runs) == 1
                and len(roots) == 1
                and runs[0][0] == trace_id
                and runs[0][1] == parent_id
                and roots[0] == (trace_id, parent_id)
            )
            failures += not check(linked, "remote trace relationship", f"runs={runs}, roots={roots}")
        except sqlite3.DatabaseError as error:
            failures += not check(False, "remote trace valid database", str(error))
    else:
        failures += not check(False, "remote trace SQLite file created")

    error_db = WORK / "prototype-error.sqlite"
    error_db.unlink(missing_ok=True)
    bad, bad_output = compile_source(
        compiler, "bad_proto_mismatch", ["--db", str(error_db), "--summary", "--color=never"]
    )
    failures += not check(
        bad.returncode != 0 and bad.stdout == "" and not bad_output.exists()
        and bool(re.search(r":\d+:\d+", bad.stderr)) and "^" in bad.stderr
        and "summary" in bad.stderr.lower() and "\x1b[" not in bad.stderr,
        "prototype semantic failure preserves diagnostic/summary streams",
        f"exit={bad.returncode}, stderr={bad.stderr[:180]!r}",
    )
    if error_db.exists():
        try:
            with sqlite3.connect(error_db) as conn:
                run_status = conn.execute("SELECT status FROM run").fetchall()
                semantic_status = conn.execute("SELECT status FROM span WHERE name='semantic'").fetchall()
                error_logs = conn.execute("SELECT message FROM log WHERE severity='error'").fetchall()
            failures += not check(
                run_status == [("error",)] and semantic_status == [("error",)] and bool(error_logs),
                "prototype failure persisted as error run/span/log",
                f"run={run_status}, semantic={semantic_status}, logs={error_logs[:2]}",
            )
        except sqlite3.DatabaseError as error:
            failures += not check(False, "prototype failure SQLite valid", str(error))
    else:
        failures += not check(False, "prototype failure SQLite file created")

    optimized_db = WORK / "optimized-run.sqlite"
    optimized_db.unlink(missing_ok=True)
    optimized, _ = compile_source(compiler, "mem2reg_defined_path", ["-O1", "--db", str(optimized_db)])
    failures += not check(optimized.returncode == 0, "optimized telemetry invocation", optimized.stderr)
    if optimized_db.exists():
        try:
            with sqlite3.connect(optimized_db) as conn:
                pass_metrics = conn.execute(
                    "SELECT attributes_json FROM metric WHERE name='compiler.pass.duration'"
                ).fetchall()
            has_mem2reg = any(json.loads(row[0]).get("pass") == "mem2reg" for row in pass_metrics)
            failures += not check(has_mem2reg, "O1 mem2reg pass duration persisted", f"metrics={len(pass_metrics)}")
        except (sqlite3.DatabaseError, ValueError) as error:
            failures += not check(False, "optimized telemetry database valid", str(error))
    else:
        failures += not check(False, "optimized telemetry SQLite file created")

    # A database path that aliases source or assembly must be rejected before
    # opening either file; otherwise sqlite or output replacement destroys it.
    # 数据库路径若别名到源码/汇编，必须在触碰文件前拒绝，避免数据丢失。
    source = SOURCES / "basic.sy"
    source_bytes = source.read_bytes()
    source_output = WORK / "source-alias.s"
    source_output.unlink(missing_ok=True)
    for label, db_path in (
        ("source exact", str(source)),
        ("source dot alias", str(SOURCES) + "/./basic.sy"),
    ):
        result = run([str(compiler), str(source), "-o", str(source_output), "--db", db_path])
        failures += not check(
            result.returncode == 2 and source.read_bytes() == source_bytes and not source_output.exists(),
            f"reject DB {label} without file changes", f"exit={result.returncode}, stderr={result.stderr[:160]!r}",
        )

    output_alias = WORK / "output-alias.s"
    output_alias.write_text(sentinel)
    for label, db_path in (
        ("output exact", str(output_alias)),
        ("output dot alias", str(WORK) + "/./output-alias.s"),
    ):
        result = run([str(compiler), str(source), "-o", str(output_alias), "--db", db_path])
        failures += not check(
            result.returncode == 2 and output_alias.read_text() == sentinel and source.read_bytes() == source_bytes,
            f"reject DB {label} without file changes", f"exit={result.returncode}, stderr={result.stderr[:160]!r}",
        )

    if os.name == "posix":
        source_link = WORK / "source-link.sy"
        source_link.unlink(missing_ok=True)
        source_link.symlink_to(source)
        result = run([str(compiler), str(source), "-o", str(source_output), "--db", str(source_link)])
        failures += not check(
            result.returncode == 2 and source.read_bytes() == source_bytes and not source_output.exists(),
            "reject DB source symlink without file changes",
        )

        output_link = WORK / "output-hardlink.s"
        output_link.unlink(missing_ok=True)
        os.link(output_alias, output_link)
        result = run([str(compiler), str(source), "-o", str(output_alias), "--db", str(output_link)])
        failures += not check(
            result.returncode == 2 and output_alias.read_text() == sentinel and output_link.read_text() == sentinel,
            "reject DB output hardlink without file changes",
        )

    print(f"RESULT {failures} failed assertion(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
