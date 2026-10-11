#!/usr/bin/env python3
"""Elaborate the RTL and run self-checking simulations; optionally synthesise it."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
TEST_DIRECTORY = Path(__file__).resolve().parent
MODULE_PATTERN = re.compile(r"^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)\b", re.MULTILINE)


def resolve_tool(variable, command, local_path):
    configured = os.environ.get(variable)
    candidates = [configured] if configured else [shutil.which(command), str(local_path)]
    for candidate in candidates:
        if not candidate:
            continue
        located = shutil.which(os.path.expanduser(candidate))
        if located:
            return Path(located).resolve()
    hint = f"Set {variable} to an executable path or install {command} locally."
    raise RuntimeError(f"{command} is unavailable. {hint}")


def compiler_arguments(iverilog):
    configured = os.environ.get("IVERILOG_BASE")
    if configured:
        base = Path(configured).expanduser().resolve()
        if not (base / "ivl").is_file() or not (base / "ivlpp").is_file():
            raise RuntimeError(f"IVERILOG_BASE does not contain ivl and ivlpp: {base}")
        return ["-B", str(base)]

    # A locally extracted Debian package needs its relocated compiler components.
    prefix = iverilog.parent.parent
    candidates = [prefix / "lib/ivl", *sorted((prefix / "lib").glob("*/ivl"))]
    for base in candidates:
        if (base / "ivl").is_file() and (base / "ivlpp").is_file():
            return ["-B", str(base)]
    return []


def text_output(value):
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""


def run_command(command, log_path, timeout, environment):
    try:
        result = subprocess.run(
            [str(part) for part in command], cwd=REPOSITORY, env=environment,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=timeout, check=False,
        )
        code = result.returncode
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired as error:
        code = 124
        output = text_output(error.stdout) + text_output(error.stderr)
        output += f"\nTimed out after {timeout:g} seconds.\n"
    except OSError as error:
        code = 127
        output = str(error) + "\n"
    log_path.write_text(output, encoding="utf-8")
    return {"command": [str(part) for part in command], "exit_code": code,
            "log": str(log_path), "output": output}


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rtl-dir", type=Path, default=REPOSITORY / "src/nexus/fpga")
    parser.add_argument("--build-dir", type=Path, default=REPOSITORY / "build-fpga-tests")
    parser.add_argument("--bench", action="append", metavar="TOP",
                        help="run only this testbench top; repeat to select several")
    parser.add_argument("--timeout", type=float, default=60,
                        help="timeout in seconds for each tool invocation (default: 60)")
    parser.add_argument("--synth", action="store_true",
                        help="also run Yosys generic synthesis and structural checks")
    args = parser.parse_args()
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout must be finite and positive")
    args.rtl_dir = args.rtl_dir.expanduser().resolve()
    args.build_dir = args.build_dir.expanduser().resolve()
    return args


def main():
    args = parse_arguments()
    # Portable components share one source set. Vendor/HLS adapters have separate
    # recipes and must not be swept into a generic simulator or Yosys invocation.
    sources = sorted(args.rtl_dir.glob("*.sv"))
    sources += sorted((args.rtl_dir / "market_data").glob("*.sv"))
    if not sources:
        raise RuntimeError(f"No SystemVerilog sources found in {args.rtl_dir}")
    available_benches = {path.stem: path for path in sorted(TEST_DIRECTORY.glob("tb_*.sv"))}
    selected = sorted(set(args.bench)) if args.bench else sorted(available_benches)
    if not selected:
        raise RuntimeError(f"No tb_*.sv testbenches found in {TEST_DIRECTORY}")
    unknown = set(selected) - available_benches.keys()
    if unknown:
        raise RuntimeError("Unknown testbench: " + ", ".join(sorted(unknown)))

    modules = []
    for source in sources:
        names = MODULE_PATTERN.findall(source.read_text(encoding="utf-8"))
        if not names:
            raise RuntimeError(f"No module declaration found in {source}")
        modules.extend(names)
    if len(modules) != len(set(modules)):
        raise RuntimeError("Duplicate RTL module names")
    for name in selected:
        declarations = MODULE_PATTERN.findall(available_benches[name].read_text(encoding="utf-8"))
        if name not in declarations:
            raise RuntimeError(f"{available_benches[name]} does not define top {name}")

    iverilog = resolve_tool("IVERILOG", "iverilog", REPOSITORY / ".tools/iverilog/usr/bin/iverilog")
    vvp = resolve_tool("VVP", "vvp", REPOSITORY / ".tools/iverilog/usr/bin/vvp")
    ivl_arguments = compiler_arguments(iverilog)
    environment = dict(os.environ)
    tools = {"iverilog": str(iverilog), "vvp": str(vvp), "iverilog_arguments": ivl_arguments}
    yosys = None
    if args.synth:
        yosys = resolve_tool("YOSYS", "yosys", REPOSITORY / ".tools/pylib/bin/yowasp-yosys")
        tools["yosys"] = str(yosys)
        if yosys == (REPOSITORY / ".tools/pylib/bin/yowasp-yosys").resolve():
            local_python = str(REPOSITORY / ".tools/pylib")
            previous = environment.get("PYTHONPATH")
            environment["PYTHONPATH"] = local_python + (os.pathsep + previous if previous else "")

    args.build_dir.mkdir(parents=True, exist_ok=True)
    tracked_sources = [*sources, *(available_benches[name] for name in selected)]
    report = {
        "tools": tools,
        "timeout_seconds": args.timeout,
        "elaboration_count": len(modules),
        "simulation_case_count": len(selected),
        "synthesis_enabled": args.synth,
        "source_sha256": {
            str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in tracked_sources
        },
        "runs": [],
    }
    failed = False

    def check(label, command, log_name, success_marker=None):
        nonlocal failed
        result = run_command(command, args.build_dir / log_name, args.timeout, environment)
        output = result.pop("output")
        if (result["exit_code"] == 0 and success_marker is not None and
                success_marker not in output.splitlines()):
            result["exit_code"] = 1
            output += f"\nMissing completion marker: {success_marker}\n"
            Path(result["log"]).write_text(output, encoding="utf-8")
        result["check"] = label
        report["runs"].append(result)
        passed = result["exit_code"] == 0
        failed |= not passed
        print(f"[{'PASS' if passed else 'FAIL'}] {label}", flush=True)
        if not passed:
            print("\n".join(output.splitlines()[-40:]), flush=True)
        return passed

    check("Icarus compiler version", [iverilog, *ivl_arguments, "-V"], "iverilog-version.log")
    check("Icarus runtime version", [vvp, "-V"], "vvp-version.log")
    for module in modules:
        check(
            f"elaborate {module}",
            [iverilog, *ivl_arguments, "-g2012", "-Wall", "-s", module,
             "-o", args.build_dir / f"{module}.elaborated.vvp", *sources],
            f"{module}.elaborate.log",
        )
    for name in selected:
        binary = args.build_dir / f"{name}.vvp"
        compiled = check(
            f"compile {name}",
            [iverilog, *ivl_arguments, "-g2012", "-Wall", "-s", name,
             "-o", binary, available_benches[name], *sources],
            f"{name}.compile.log",
        )
        if compiled:
            check(f"simulate {name}", [vvp, binary], f"{name}.simulate.log",
                  success_marker=f"RTL_TEST_PASS: {name}")

    if yosys is not None:
        check("Yosys version", [yosys, "-V"], "yosys-version.log")
        def quote(path):
            return '"' + str(path).replace("\\", "\\\\").replace('"', '\\"') + '"'
        read_sources = " ".join(quote(path) for path in sources)
        for module in modules:
            script = (
                f"read_verilog -sv {read_sources}; hierarchy -check -top {module}; "
                f"synth -top {module}; check -assert; stat"
            )
            check(f"synthesise {module}", [yosys, "-p", script], f"{module}.synth.log")

    changed = [
        str(path) for path in tracked_sources
        if hashlib.sha256(path.read_bytes()).hexdigest() != report["source_sha256"][str(path)]
    ]
    if changed:
        failed = True
        report["changed_during_run"] = changed
        print("Sources changed during verification: " + ", ".join(changed), file=sys.stderr)
    report["status"] = "failed" if failed else "passed"
    summary = args.build_dir / "rtl-results.json"
    summary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"RTL checks {report['status']}: {len(modules)} RTL modules, "
          f"{len(selected)} simulation cases, synthesis={'on' if args.synth else 'off'}")
    print(f"Logs and source hashes: {summary}")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"RTL checks failed: {error}", file=sys.stderr)
        sys.exit(1)
