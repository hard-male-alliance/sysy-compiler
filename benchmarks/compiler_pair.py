#!/usr/bin/env python3
"""Compare two compiler builds with paired fresh-process compilation runs.

中文：两个编译器共享同一源码和输出路径，AB/BA 顺序交替，避免路径/时序混杂。
English: Both compilers use the same input/output paths; alternating AB/BA limits drift.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

from compile_time import ROOT, digest, ensure_local_output, run_once, stats, workload


def main() -> int:
    """运行两个编译器的配对端到端基准。 / Run paired end-to-end compiler timings."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path,
                        default=ROOT / ".temp" / "performance" / "compiler_pair")
    parser.add_argument("--repetitions", type=int, default=31)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--functions", type=int, default=120)
    parser.add_argument("--statements", type=int, default=16)
    args = parser.parse_args()
    if args.repetitions < 21 or args.warmups < 1:
        parser.error("repetitions >= 21 and warmups >= 1 required")
    baseline = args.baseline.resolve(strict=True)
    candidate = args.candidate.resolve(strict=True)
    out = ensure_local_output(args.out)
    report: dict[str, object] = {
        "method": "paired alternating AB/BA fresh-process runs; identical input/output paths",
        "platform": platform.platform(), "processor": platform.processor(),
        "cpu_count": os.cpu_count(), "python": sys.version.split()[0],
        "baseline": str(baseline), "baseline_sha256": digest(baseline),
        "candidate": str(candidate), "candidate_sha256": digest(candidate),
        "warmups": args.warmups, "repetitions": args.repetitions,
        "cases": {},
    }
    for case in ("tiny", "functions"):
        source = out / f"{case}.sy"
        source.write_bytes(workload(case, args.functions, args.statements).encode("utf-8"))
        for opt in ("-O0", "-O2"):
            output = out / f"{case}-{opt[1:].lower()}.s"
            if output.exists():
                output.unlink()
            commands = {"baseline": [str(baseline), opt, "-o", str(output), str(source)],
                        "candidate": [str(candidate), opt, "-o", str(output), str(source)]}
            samples: dict[str, list[float]] = {"baseline": [], "candidate": []}
            for index in range(args.warmups + args.repetitions):
                order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
                for name in order:
                    elapsed, code = run_once(commands[name], ROOT)
                    if code or not output.is_file() or output.stat().st_size == 0:
                        detail = subprocess.run(commands[name], cwd=ROOT, capture_output=True,
                                                text=True, encoding="utf-8", errors="replace",
                                                check=False)
                        raise RuntimeError(f"{case} {opt} {name}: exit={code}, output={output}\n"
                                           f"{detail.stderr[-4000:]}")
                    if index >= args.warmups:
                        samples[name].append(elapsed)
            deltas = [new - old for old, new in zip(samples["baseline"], samples["candidate"])]
            key = f"{case}:{opt}"
            report["cases"][key] = {
                "input_bytes": source.stat().st_size, "input_sha256": digest(source),
                "output_path": str(output), "commands": commands,
                "baseline_ms": samples["baseline"], "candidate_ms": samples["candidate"],
                "baseline_latency": stats(samples["baseline"]),
                "candidate_latency": stats(samples["candidate"]),
                "paired_delta_ms": deltas, "paired_delta": stats(deltas),
            }
            data = report["cases"][key]
            print(f"{key:15s} baseline {data['baseline_latency']['median_ms']:.2f} ms  "
                  f"candidate {data['candidate_latency']['median_ms']:.2f} ms  "
                  f"delta {data['paired_delta']['median_ms']:+.2f} ms")
    target = out / "results.json"
    target.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Raw results: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
