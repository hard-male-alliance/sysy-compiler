#!/usr/bin/env python3
"""Measure the compiler's in-process no-database summary on a tiny SysY file.

中文：这是诊断探针，不把管道捕获的外部时间与主基准直接比较。
English: This diagnostic probe captures stderr; its outer latency is not the main benchmark.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

from compile_time import ROOT, digest, ensure_local_output, stats, workload


ELAPSED = re.compile(r"^\s*(?:total|elapsed)\s+([0-9.]+) ms$", re.MULTILINE)
STAGE = re.compile(r"^\s*(parse|semantic|lower|optimize|codegen|publish)\s+([0-9.]+) ms$",
                   re.MULTILINE)
DATABASE = re.compile(r"^\s*(setup|flush|close)\s+([0-9.]+) ms$", re.MULTILINE)


def main() -> int:
    """采样摘要内部计时，并保留原始值。 / Sample and preserve in-process timings."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / ".temp" / "performance" / "summary_probe")
    parser.add_argument("--repetitions", type=int, default=31)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--case", choices=("tiny", "functions"), default="tiny")
    parser.add_argument("--functions", type=int, default=120)
    parser.add_argument("--statements", type=int, default=16)
    parser.add_argument("--db", action="store_true")
    args = parser.parse_args()
    if args.repetitions < 3 or args.warmups < 0:
        parser.error("repetitions >= 3 and warmups >= 0 required")
    compiler = args.compiler.resolve(strict=True)
    out = ensure_local_output(args.out)
    source = out / f"{args.case}.sy"
    source.write_bytes(workload(args.case, args.functions, args.statements).encode("utf-8"))
    command = [str(compiler), "-O0", "-o", str(out / f"{args.case}.s"), str(source),
               "--summary", "--color=never"]
    database = out / f"{args.case}.sqlite"
    if args.db:
        for suffix in ("", "-wal", "-shm"):
            candidate = Path(str(database) + suffix)
            if candidate.exists():
                candidate.unlink()
        command.append(f"--db={database}")
    totals: list[float] = []
    stages: dict[str, list[float]] = {}
    lifecycle: dict[str, list[float]] = {}
    for index in range(args.warmups + args.repetitions):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=False)
        match = ELAPSED.search(result.stderr)
        if result.returncode or not match:
            raise RuntimeError(f"summary failed: exit={result.returncode}\n{result.stderr}")
        if index < args.warmups:
            continue
        totals.append(float(match.group(1)))
        for name, duration in STAGE.findall(result.stderr):
            stages.setdefault(name, []).append(float(duration))
        for name, duration in DATABASE.findall(result.stderr):
            lifecycle.setdefault(name, []).append(float(duration))
    if args.db and any(len(lifecycle.get(name, [])) != args.repetitions
                       for name in ("setup", "flush", "close")):
        raise RuntimeError("--db requested but setup/flush/close lines were not collected")
    report = {"compiler": str(compiler), "compiler_sha256": digest(compiler),
              "command": command, "input_sha256": digest(source),
              "total_samples_ms": totals, "total": stats(totals),
              "shown_stage_samples_ms": stages,
              "shown_stage_medians_ms": {name: stats(values)["median_ms"]
                                         for name, values in stages.items()},
              "sqlite_lifecycle_samples_ms": lifecycle,
              "sqlite_lifecycle": {name: stats(values) for name, values in lifecycle.items()}}
    target = out / "results.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"reported in-process elapsed median {report['total']['median_ms']:.2f} ms; raw {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
