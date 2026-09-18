#!/usr/bin/env python3
"""Repeat workloads in fresh processes; retain raw samples and report median timing/RSS."""
import argparse
import json
import pathlib
import statistics
import subprocess
import tempfile
from datetime import datetime, timezone


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build-release/lakestore_bench")
    parser.add_argument("--rows", type=int, default=262144)
    parser.add_argument("--memory-rows", type=int, default=1048576)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--report", default="docs/WORKLOAD_BENCHMARK.json")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("repeats must be positive")
    binary = str(pathlib.Path(args.binary).resolve())
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "seed": 98231,
              "repeats": args.repeats, "backend": "mixed; scan workloads local-filesystem, contention/startup/CRC memory",
              "workloads": [], "memory": [], "crc": [], "contention": []}

    def run(*arguments):
        output = subprocess.run([binary, *map(str, arguments)], check=True, capture_output=True,
                                text=True, timeout=180)
        return json.loads(output.stdout)

    def summarize(samples):
        return {"median_microseconds": statistics.median(s["microseconds"] for s in samples),
                "median_peak_rss_bytes": statistics.median(s["peak_rss_bytes"] for s in samples),
                "samples": samples}

    with tempfile.TemporaryDirectory(prefix="lakestore-bench-") as temporary:
        for layout in ("sorted", "shuffled", "skewed"):
            path = pathlib.Path(temporary) / layout
            run("--mode", "prepare", "--path", path, "--layout", layout, "--rows", args.rows)
            predicates = [f"id < {max(1, args.rows // 100)}", "id >= 0"] if layout != "skewed" else ["id > 0", "id = 0"]
            for predicate in predicates:
                expected = None
                for parallelism in (1, 4):
                    for prune in (False, True):
                        options = ["--mode", "scan", "--path", path, "--layout", layout,
                                   "--rows", args.rows, "--where", predicate,
                                   "--parallelism", parallelism, "--buffer-mib", 8]
                        if not prune:
                            options.append("--no-prune")
                        samples = [run(*options) for _ in range(args.repeats)]
                        for sample in samples:
                            fingerprint = (sample["rows_returned"], sample["checksum"])
                            if expected is None:
                                expected = fingerprint
                            assert fingerprint == expected, (layout, predicate, sample)
                            assert sample["peak_reserved_bytes"] <= sample["buffer_budget_bytes"]
                        report["workloads"].append({"layout": layout, "predicate": predicate,
                                                     "parallelism": parallelism, "prune": prune,
                                                     **summarize(samples)})
        path = pathlib.Path(temporary) / "memory"
        run("--mode", "prepare", "--path", path, "--layout", "sorted", "--rows", args.memory_rows)
        for materialize in (False, True):
            options = ["--mode", "scan", "--path", path, "--rows", args.memory_rows,
                       "--where", "id >= 0", "--parallelism", 4, "--buffer-mib", 8]
            if materialize:
                options.append("--materialize")
            samples = [run(*options) for _ in range(args.repeats)]
            assert all(s["rows_returned"] == args.memory_rows for s in samples)
            report["memory"].append({"materialized": materialize,
                                      "logical_result_bytes": args.memory_rows * (128 + 8),
                                      **summarize(samples)})
        for writers in (1, 4, 16):
            samples = [run("--mode", "contention", "--parallelism", writers) for _ in range(args.repeats)]
            report["contention"].append({"writers": writers, **summarize(samples)})
        report["startup"] = run("--mode", "startup", "--rows", args.rows)
        report["crc"] = [run("--mode", "crc") for _ in range(args.repeats)]
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    pathlib.Path(args.report).write_text(json.dumps(report, indent=2) + "\n")
    print(f"Recorded {len(report['workloads'])} workload configurations with {args.repeats} repetitions")


if __name__ == "__main__":
    main()
