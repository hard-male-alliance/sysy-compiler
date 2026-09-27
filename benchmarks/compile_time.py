#!/usr/bin/env python3
"""Measure fresh-process SysY compilation on reproducible synthetic workloads.

中文：只将输入、汇编、数据库和原始结果写入仓库内的 .temp/performance。
English: Inputs, assembly, databases, and raw results stay in .temp/performance.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import random
import sqlite3
import statistics
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / ".temp" / "performance"


def workload(name: str, functions: int, statements: int) -> str:
    """Generate deterministic, valid SysY without relying on undefined behavior.

    中文：函数压力用独立调用与局部变量，数组压力用有界循环和固定形状。
    English: Function stress uses independent calls/locals; array stress uses bounded loops.
    """
    if name == "tiny":
        return "int main() { return 0; }\n"
    if name == "functions":
        parts = []
        for index in range(functions):
            parts.append(f"int f{index}(int x) {{\n  int a = x;\n")
            for step in range(statements):
                parts.append(f"  a = a + {step % 7 + 1};\n")
            parts.append("  return a;\n}\n")
        parts.append("int main() {\n  int x = 0;\n")
        for index in range(functions):
            parts.append(f"  x = f{index}(x);\n")
        parts.append("  return x;\n}\n")
        return "".join(parts)
    if name == "arrays":
        side = max(2, min(128, functions))
        parts = [f"int a[{side}][{side}];\n", "int main() {\n",
                 "  int i = 0;\n  int j = 0;\n  int s = 0;\n",
                 f"  while (i < {side}) {{\n    j = 0;\n",
                 f"    while (j < {side}) {{\n",
                 "      a[i][j] = i + j;\n      s = s + a[i][j];\n",
                 "      j = j + 1;\n    }\n    i = i + 1;\n  }\n"]
        for index in range(statements):
            parts.append(f"  s = s + a[{index % side}][{(index * 7) % side}];\n")
        parts.append("  return s;\n}\n")
        return "".join(parts)
    raise ValueError(name)


def digest(path: Path) -> str:
    """绑定测量值与文件内容。 / Bind measurements to artifact SHA-256 hashes."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values: list[float]) -> dict[str, float]:
    """汇总延迟分布；自助法区间只针对中位数。 / Bootstrap the median."""
    ordered = sorted(values)
    rng = random.Random(0x5A5A)
    boot = sorted(statistics.median(rng.choices(values, k=len(values))) for _ in range(2000))
    return {
        "n": len(values),
        "median_ms": statistics.median(values),
        "median_ci95_low_ms": boot[50],
        "median_ci95_high_ms": boot[1949],
        "p95_ms": ordered[min(len(values) - 1, (95 * len(values) + 99) // 100 - 1)],
        "min_ms": ordered[0],
        "max_ms": ordered[-1],
    }


def make_command(template: list[str], mapping: dict[str, str]) -> list[str]:
    """展开路径而不启动 shell。 / Expand paths without changing argument boundaries."""
    return [arg.format_map(mapping) for arg in template]


def run_once(command: list[str], cwd: Path) -> tuple[float, int]:
    """计量进程创建至退出，屏蔽终端成本。 / Time process start through exit."""
    start = time.perf_counter_ns()
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, check=False)
    elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
    return elapsed_ms, result.returncode


def stage_rows(database: Path, measured_count: int) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """读取测量区间的 span，不累加嵌套时长。 / Read measured runs' spans."""
    if not database.exists():
        return [], []
    with sqlite3.connect(database) as connection:
        try:
            runs = connection.execute(
                "SELECT run_id, duration_ns FROM run WHERE status != 'running' "
                "ORDER BY started_unix_ns, rowid"
            ).fetchall()[-measured_count:]
            records = []
            for run_id, run_ns in runs:
                records.extend((run_id, run_ns, name, stage_ns) for name, stage_ns in
                               connection.execute("SELECT name, duration_ns FROM span "
                                                  "WHERE run_id = ? ORDER BY started_unix_ns",
                                                  (run_id,)))
        except sqlite3.Error:
            return [], []
    rows = [{"run_id": run_id, "run_ms": run_ns / 1e6,
             "stage": name, "stage_ms": stage_ns / 1e6}
            for run_id, run_ns, name, stage_ns in records]
    grouped: dict[str, list[float]] = {}
    for row in rows:
        if row["stage"] == "compiler.run":
            continue
        grouped.setdefault(str(row["stage"]), []).append(float(row["stage_ms"]))
    summary = [{"stage": name, "observations": len(values),
                "median_ms": statistics.median(values)}
               for name, values in grouped.items()]
    summary.sort(key=lambda item: float(item["median_ms"]), reverse=True)
    return rows, summary


def ensure_local_output(output: Path) -> Path:
    """拒绝仓库 .temp 外的输出位置。 / Restrict outputs to the repository's .temp."""
    output = output.resolve()
    if not output.is_relative_to((ROOT / ".temp").resolve()):
        raise ValueError("--out must remain inside this repository's .temp directory")
    output.mkdir(parents=True, exist_ok=True)
    return output


def main() -> int:
    """生成输入、逐次启动编译器并保存 JSON。 / Run fresh processes and save raw JSON."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--repetitions", type=int, default=25)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--functions", type=int, default=120)
    parser.add_argument("--statements", type=int, default=16)
    parser.add_argument("--telemetry", action="store_true")
    parser.add_argument("--telemetry-arg", default="--db={database}")
    parser.add_argument("--opt", default="-O0")
    args = parser.parse_args()
    if args.repetitions < 3 or args.warmups < 0 or args.functions < 1 or args.statements < 0:
        parser.error("repetitions >= 3, warmups >= 0, functions >= 1, statements >= 0 required")
    compiler = args.compiler.resolve(strict=True)
    out = ensure_local_output(args.out)
    report: dict[str, object] = {
        "method": "fresh process each sample; warm OS filesystem cache; no forced cold disk cache",
        "platform": platform.platform(), "python": sys.version.split()[0],
        "processor": platform.processor(), "cpu_count": os.cpu_count(),
        "compiler": str(compiler), "compiler_sha256": digest(compiler),
        "arguments": vars(args) | {"compiler": str(compiler), "out": str(out)},
        "workloads": {},
    }
    control: list[float] = []
    for index in range(args.warmups + args.repetitions):
        elapsed, code = run_once([str(compiler), "--version"], ROOT)
        if code:
            raise RuntimeError(f"compiler --version returned {code}")
        if index >= args.warmups:
            control.append(elapsed)
    report["process_control"] = {"command": [str(compiler), "--version"],
                                 "samples_ms": control, "latency": stats(control)}
    print(f"{'version':10s} {'(control)':>8s}  "
          f"median {report['process_control']['latency']['median_ms']:.2f} ms")
    for name in ("tiny", "functions", "arrays"):
        source = out / f"{name}.sy"
        source.write_bytes(workload(name, args.functions, args.statements).encode("utf-8"))
        assembly = out / f"{name}.s"
        database = out / f"{name}.sqlite"
        if args.telemetry:
            for suffix in ("", "-wal", "-shm"):
                candidate = Path(str(database) + suffix)
                if candidate.exists():
                    candidate.unlink()
        mapping = {"input": str(source), "output": str(assembly),
                   "database": str(database), "compiler": str(compiler)}
        command = make_command(["{compiler}", args.opt, "-o", "{output}", "{input}"], mapping)
        if args.telemetry:
            command.append(args.telemetry_arg.format_map(mapping))
        samples: list[float] = []
        first: float | None = None
        for index in range(args.warmups + args.repetitions):
            elapsed, code = run_once(command, ROOT)
            if code:
                debug = subprocess.run(command, cwd=ROOT, capture_output=True,
                                       text=True, encoding="utf-8", errors="replace", check=False)
                raise RuntimeError(f"{name}: compiler returned {code}\n{debug.stderr[-4000:]}")
            if not assembly.is_file():
                raise RuntimeError(f"{name}: compiler exited 0 but did not produce {assembly}")
            if index == 0:
                first = elapsed
            if index >= args.warmups:
                samples.append(elapsed)
        rows, stage_summary = stage_rows(database, args.repetitions) if args.telemetry else ([], [])
        if args.telemetry and not rows:
            raise RuntimeError(f"{name}: --telemetry requested but SQLite contains no completed spans")
        result = {"input_bytes": source.stat().st_size, "input_sha256": digest(source),
                  "assembly_bytes": assembly.stat().st_size,
                  "assembly_sha256": digest(assembly), "command": command,
                  "first_process_ms": first, "samples_ms": samples,
                  "latency": stats(samples), "stages": rows,
                  "stage_medians": stage_summary}
        report["workloads"][name] = result
        print(f"{name:10s} {result['input_bytes']:>8} B  "
              f"median {result['latency']['median_ms']:.2f} ms  "
              f"p95 {result['latency']['p95_ms']:.2f} ms")
    report_path = out / "results.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Raw results: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
