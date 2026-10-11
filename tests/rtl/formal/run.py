#!/usr/bin/env python3
"""Run the optional formal safety and reachability tasks with SymbiYosys."""

import argparse
import json
import math
import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CASES = ("arbiter", "network", "mold", "sbe", "loopback", "session_gate")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-rtl-formal")
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--mode", choices=("all", "prove", "cover"), default="all")
    parser.add_argument("--timeout", type=float, default=120,
                        help="wall-clock limit per SBY task, in seconds")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    sby = shutil.which(os.environ.get("SBY", "sby"))
    if sby is None:
        raise RuntimeError("SymbiYosys is unavailable; add it to PATH or set SBY")
    for tool in ("yosys", "yosys-smtbmc", "z3"):
        if shutil.which(tool) is None:
            raise RuntimeError(f"Required formal tool is unavailable: {tool}")
    output = args.build_dir.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    modes = ("prove", "cover") if args.mode == "all" else (args.mode,)
    report = []
    for name in dict.fromkeys(args.case or CASES):
        for mode in modes:
            task_dir = (output / f"{name}-{mode}").resolve()
            # SBY removes its work directory with -f; confine it to the selected build root.
            if not task_dir.is_relative_to(output):
                raise RuntimeError("Formal work directory escapes the selected build directory")
            command = [sby, "-f", "-d", str(task_dir), str(HERE / f"{name}.sby"), mode]
            status_file = task_dir / "status"
            status_file.unlink(missing_ok=True)
            log = output / f"{name}-{mode}.log"
            timed_out = False
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.Popen(command, cwd=HERE, stdout=stream,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    # Terminate the solver descendants as well as the SBY coordinator.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    code = 124
                    timed_out = True
            status = status_file.read_text(encoding="utf-8").strip() if status_file.is_file() else ""
            passed = code == 0 and status == "PASS"
            report.append({"case": name, "mode": mode, "command": command,
                           "exit_code": code, "status": status, "passed": passed,
                           "timed_out": timed_out, "log": str(log)})
            (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(f"[{'PASS' if passed else 'FAIL'}] {name}/{mode}: {status or 'no status'}", flush=True)
    return 0 if all(item["passed"] for item in report) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"Formal verification failed: {error}", file=sys.stderr)
        sys.exit(1)
