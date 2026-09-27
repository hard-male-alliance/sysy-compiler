#!/usr/bin/env python3
"""Run a randomized four-way short-process SQLite lifecycle comparison.

中文：比较 WAL/DELETE 日志模式与每次执行 DDL/版本号快速路径；输出仅位于 .temp。
English: Compare WAL/DELETE and repeated DDL/version fast path inside repository .temp.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import random
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

from compile_time import ROOT, digest, ensure_local_output, stats


FIELDS = ("setup_ms", "flush_ms", "close_ms", "inside_ms", "external_ms")
MODES = (("WAL", "always"), ("WAL", "version"),
         ("DELETE", "always"), ("DELETE", "version"))


def launch(probe: Path, journal: str, schema: str, database: Path,
           unique_id: str) -> dict[str, float | str]:
    """启动一次短进程并校验 JSON。 / Launch one process and validate its JSON timing."""
    command = [str(probe), journal, schema, str(database), unique_id]
    started = time.perf_counter_ns()
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", check=False)
    external = (time.perf_counter_ns() - started) / 1_000_000
    if result.returncode:
        raise RuntimeError(f"probe failed: {command}\n{result.stderr}")
    value = json.loads(result.stdout)
    if any(field not in value for field in FIELDS[:-1]):
        raise RuntimeError(f"missing timing in probe output: {result.stdout}")
    value["external_ms"] = external
    return value


def paired(left: list[dict[str, float | str]], right: list[dict[str, float | str]]) -> dict:
    """按相同轮次计算差值及不确定度。 / Compute within-round differences and uncertainty."""
    return {field: stats([float(a[field]) - float(b[field]) for a, b in zip(left, right)])
            for field in FIELDS}


def main() -> int:
    """交错四种模式并保留全部原始测量。 / Interleave four modes and save all samples."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / ".temp" / "performance" / "sqlite_micro")
    parser.add_argument("--repetitions", type=int, default=41)
    parser.add_argument("--warmups", type=int, default=5)
    args = parser.parse_args()
    if args.repetitions < 5 or args.warmups < 1:
        parser.error("repetitions >= 5 and warmups >= 1 required")
    probe = args.probe.resolve(strict=True)
    out = ensure_local_output(args.out)
    databases = {mode: out / f"{mode[0].lower()}-{mode[1]}.sqlite" for mode in MODES}
    for database in databases.values():
        for suffix in ("", "-wal", "-shm"):
            candidate = Path(str(database) + suffix)
            if candidate.exists():
                candidate.unlink()
    samples: dict[tuple[str, str], list[dict[str, float | str]]] = {mode: [] for mode in MODES}
    rng = random.Random(0x51A17E)
    for index in range(args.warmups + args.repetitions):
        order = list(MODES)
        rng.shuffle(order)
        for journal, schema in order:
            mode = (journal, schema)
            value = launch(probe, journal, schema, databases[mode],
                           f"{journal.lower()}_{schema}_{index}")
            if index >= args.warmups:
                samples[mode].append(value)
    journal_modes = {}
    for mode, database in databases.items():
        with sqlite3.connect(database) as connection:
            journal_modes[f"{mode[0]}-{mode[1]}"] = connection.execute(
                "PRAGMA journal_mode").fetchone()[0].upper()
        if journal_modes[f"{mode[0]}-{mode[1]}"] != mode[0]:
            raise RuntimeError(f"journal mode mismatch for {database}")
    summaries = {f"{journal}-{schema}":
                 {field: stats([float(row[field]) for row in samples[(journal, schema)]])
                  for field in FIELDS}
                 for journal, schema in MODES}
    contrasts = {}
    for journal in ("WAL", "DELETE"):
        contrasts[f"{journal}:always-minus-version"] = paired(samples[(journal, "always")],
                                                                 samples[(journal, "version")])
    for schema in ("always", "version"):
        contrasts[f"{schema}:WAL-minus-DELETE"] = paired(samples[("WAL", schema)],
                                                          samples[("DELETE", schema)])
    report = {
        "method": "one SQLite connection and run per new process; seeded randomized four-way order",
        "platform": platform.platform(), "processor": platform.processor(),
        "cpu_count": os.cpu_count(), "python": sys.version.split()[0],
        "probe": str(probe), "probe_sha256": digest(probe),
        "sqlite_version": samples[MODES[0]][0]["sqlite_version"],
        "repetitions": args.repetitions, "warmups": args.warmups,
        "journal_modes_verified": journal_modes,
        "samples": {f"{journal}-{schema}": rows for (journal, schema), rows in samples.items()},
        "summaries": summaries, "paired_contrasts": contrasts,
    }
    target = out / "results.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for name, values in summaries.items():
        print(f"{name:15s} setup {values['setup_ms']['median_ms']:.2f} ms  "
              f"close {values['close_ms']['median_ms']:.2f} ms  "
              f"external {values['external_ms']['median_ms']:.2f} ms")
    print(f"Raw results: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
