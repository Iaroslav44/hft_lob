#!/usr/bin/env python3
"""Compare pipeline throughput using sequential, randomised process runs."""

import argparse
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

MEDIAN_PATTERN = re.compile(r"\bmedian=(\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)M\b")
BUILD_KEYS = {
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_COMPILER",
    "CMAKE_CXX_FLAGS",
    "CMAKE_CXX_FLAGS_RELEASE",
    "HFT_NATIVE",
    "HFT_LTO",
    "HFT_QUEUE_CAPACITY",
    "HFT_SANITIZE",
    "HFT_TSAN",
}
RUN_TIMEOUT_SECONDS = 120


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def binary_metadata(path):
    digest = hashlib.sha256()
    with path.open("rb") as binary:
        for chunk in iter(lambda: binary.read(1024 * 1024), b""):
            digest.update(chunk)
    metadata = {"path": str(path), "sha256": digest.hexdigest(), "build_config": None}
    for directory in (path.parent, path.parent.parent):
        cache = directory / "CMakeCache.txt"
        if not cache.is_file():
            continue
        values = {}
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith(("#", "//")) or "=" not in line:
                continue
            key_type, value = line.split("=", 1)
            key = key_type.split(":", 1)[0]
            if key in BUILD_KEYS:
                values[key] = value
        metadata["build_config"] = {"cache_path": str(cache), "values": values}
        break
    return metadata


def save_report(path, report):
    # Checkpoint after each process without leaving a partially written JSON file.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.",
            suffix=".tmp", delete=False
        ) as output:
            temporary = Path(output.name)
            json.dump(report, output, indent=2, allow_nan=False)
            output.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def update_summary(report):
    summary = {}
    for case in report["cases"]:
        rates = [
            run["median_mmsg_per_s"] for run in report["runs"]
            if run["label"] == case["label"] and "median_mmsg_per_s" in run
        ]
        if rates:
            summary[case["label"]] = {
                "completed_processes": len(rates),
                "process_medians_mmsg_per_s": rates,
                "median_of_process_medians_mmsg_per_s": statistics.median(rates),
            }
    report["summary"] = summary


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--case", action="append", nargs=3, required=True,
        metavar=("LABEL", "EXECUTABLE", "BATCH_SIZE"),
        help="comparison case; repeat for each binary or batch configuration",
    )
    parser.add_argument("--producer-cpu", type=int, default=-1)
    parser.add_argument("--consumer-cpu", type=int, default=-1)
    parser.add_argument("--repeats", type=int, default=5, help="process runs per case (default: 5)")
    parser.add_argument("--seed", type=int, default=0, help="case-order seed (default: 0)")
    parser.add_argument("--output", type=Path, required=True, help="JSON result path")
    args = parser.parse_args()
    if args.producer_cpu < -1 or args.consumer_cpu < -1:
        parser.error("CPU indices must be -1 or nonnegative")
    if args.repeats < 1:
        parser.error("--repeats must be positive")

    cases = []
    labels = set()
    for label, executable, batch_text in args.case:
        if not label or label in labels:
            parser.error("case labels must be nonempty and unique")
        labels.add(label)
        path = Path(executable).expanduser().resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"executable is not an executable file: {path}")
        try:
            batch_size = int(batch_text, 10)
        except ValueError:
            parser.error(f"invalid batch size: {batch_text}")
        if batch_size < 1:
            parser.error("batch sizes must be positive")
        cases.append({"label": label, "batch_size": batch_size, **binary_metadata(path)})

    args.output = args.output.expanduser().resolve()
    if any(str(args.output) == case["path"] for case in cases):
        parser.error("--output must not overwrite an executable")
    return args, cases


def main():
    args, cases = parse_arguments()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    generator = random.Random(args.seed)
    schedule = []
    for repeat in range(1, args.repeats + 1):
        order = list(range(len(cases)))
        generator.shuffle(order)
        schedule.extend((repeat, index) for index in order)

    report = {
        "schema_version": 1,
        "started_utc": utc_now(),
        "status": "running",
        "metric": "median across process-reported throughput medians; not message latency",
        "configuration": {
            "producer_cpu": args.producer_cpu,
            "consumer_cpu": args.consumer_cpu,
            "repeats": args.repeats,
            "seed": args.seed,
            "timeout_seconds": RUN_TIMEOUT_SECONDS,
            "cwd": str(Path.cwd()),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "logical_cpus": os.cpu_count(),
            "allowed_cpus": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        },
        "cases": cases,
        "schedule": [{"repeat": repeat, "label": cases[index]["label"]} for repeat, index in schedule],
        "runs": [],
        "summary": {},
    }
    save_report(args.output, report)
    try:
        for position, (repeat, index) in enumerate(schedule, 1):
            case = cases[index]
            command = [
                case["path"], str(args.producer_cpu), str(args.consumer_cpu),
                str(case["batch_size"]),
            ]
            run = {
                "position": position, "repeat": repeat, "label": case["label"],
                "command": command, "started_utc": utc_now(),
            }
            report["runs"].append(run)
            started = time.perf_counter()
            try:
                completed = subprocess.run(
                    command, capture_output=True, text=True, encoding="utf-8",
                    errors="replace", timeout=RUN_TIMEOUT_SECONDS, check=False
                )
                run.update(
                    returncode=completed.returncode,
                    stdout=completed.stdout,
                    stderr=completed.stderr,
                )
            except subprocess.TimeoutExpired as error:
                # TimeoutExpired may expose bytes even when text=True.
                for stream in ("stdout", "stderr"):
                    value = getattr(error, stream) or ""
                    run[stream] = value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value
                raise RuntimeError(f"{case['label']}: process timed out") from error
            finally:
                run["wall_seconds"] = time.perf_counter() - started

            if completed.returncode != 0:
                raise RuntimeError(f"{case['label']}: exited with status {completed.returncode}")
            values = MEDIAN_PATTERN.findall(completed.stdout)
            if len(values) != 1:
                raise RuntimeError(f"{case['label']}: expected exactly one median=...M result")
            rate = float(values[0])
            if not math.isfinite(rate) or rate <= 0:
                raise RuntimeError(f"{case['label']}: median throughput must be finite and positive")
            run["reported_median"] = values[0] + "M"
            run["median_mmsg_per_s"] = rate
            update_summary(report)
            save_report(args.output, report)
            print(f"[{position}/{len(schedule)}] {case['label']}: {rate:.3f} Mmsg/s", flush=True)
    except (OSError, RuntimeError, KeyboardInterrupt) as error:
        report["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
        report["error"] = str(error) or "interrupted"
        report["finished_utc"] = utc_now()
        update_summary(report)
        save_report(args.output, report)
        print(f"Comparison {report['status']}: {report['error']}", file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1

    report["status"] = "completed"
    report["finished_utc"] = utc_now()
    save_report(args.output, report)
    for label, result in report["summary"].items():
        rate = result["median_of_process_medians_mmsg_per_s"]
        print(f"{label}: median of {result['completed_processes']} process medians = {rate:.3f} Mmsg/s")
    print(f"Results: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as error:
        print(f"Comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
