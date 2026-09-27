#!/usr/bin/env python3
"""Compare SQLite tracing overhead with interleaved compiler invocations.

中文：按 AB/BA 顺序成对运行，降低缓慢的主机负载漂移对差值的污染。
English: Alternating AB/BA pairs reduce slow host-load drift in overhead estimates.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import sys
from pathlib import Path

from compile_time import ROOT, digest, ensure_local_output, run_once, stage_rows, stats, workload


def main() -> int:
    """成对比较启停遥测的端到端耗时。 / Compare tracing on/off end-to-end latency."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / ".temp" / "performance" / "paired")
    parser.add_argument("--repetitions", type=int, default=31)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--functions", type=int, default=120)
    parser.add_argument("--statements", type=int, default=16)
    parser.add_argument("--opt", default="-O0")
    parser.add_argument("--treatment", choices=("db", "summary", "opt2"), default="db")
    args = parser.parse_args()
    if args.repetitions < 3 or args.warmups < 0:
        parser.error("repetitions >= 3 and warmups >= 0 required")
    compiler = args.compiler.resolve(strict=True)
    out = ensure_local_output(args.out)
    report: dict[str, object] = {"method": "alternating AB/BA process pairs; warm OS cache",
                                 "treatment": args.treatment,
                                 "platform": platform.platform(),
                                 "processor": platform.processor(),
                                 "cpu_count": os.cpu_count(),
                                 "python": sys.version.split()[0],
                                 "compiler": str(compiler), "compiler_sha256": digest(compiler),
                                 "workloads": {}}
    for name in ("tiny", "functions", "arrays"):
        source = out / f"{name}.sy"
        source.write_bytes(workload(name, args.functions, args.statements).encode("utf-8"))
        database = out / f"{name}.sqlite"
        for suffix in ("", "-wal", "-shm"):
            candidate = Path(str(database) + suffix)
            if candidate.exists():
                candidate.unlink()
        commands = {}
        for mode in ("off", "on"):
            assembly = out / f"{name}-{mode}.s"
            command = [str(compiler), args.opt, "-o", str(assembly), str(source)]
            if mode == "on":
                if args.treatment == "db":
                    command.append(f"--db={database}")
                elif args.treatment == "summary":
                    command.append("--summary")
                else:
                    command[1] = "-O2"
            commands[mode] = command
        samples: dict[str, list[float]] = {"off": [], "on": []}
        for index in range(args.warmups + args.repetitions):
            order = ("off", "on") if index % 2 == 0 else ("on", "off")
            for mode in order:
                elapsed, code = run_once(commands[mode], ROOT)
                if code:
                    raise RuntimeError(f"{name} {mode}: compiler returned {code}; command={commands[mode]}")
                if index >= args.warmups:
                    samples[mode].append(elapsed)
        rows, stages = stage_rows(database, args.repetitions) if args.treatment == "db" else ([], [])
        if args.treatment == "db" and not rows:
            raise RuntimeError(f"{name}: telemetry database contains no completed spans")
        deltas = [on - off for on, off in zip(samples["on"], samples["off"])]
        result = {"input_bytes": source.stat().st_size, "input_sha256": digest(source),
                  "commands": commands, "off_ms": samples["off"], "on_ms": samples["on"],
                  "off": stats(samples["off"]), "on": stats(samples["on"]),
                  "delta_ms": deltas, "delta": stats(deltas),
                  "stages": rows, "stage_medians": stages}
        report["workloads"][name] = result
        print(f"{name:10s} off {result['off']['median_ms']:.2f} ms  "
              f"on {result['on']['median_ms']:.2f} ms  "
              f"paired delta {result['delta']['median_ms']:+.2f} ms")
    target = out / "results.json"
    target.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Raw results: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
