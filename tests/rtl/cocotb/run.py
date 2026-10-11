#!/usr/bin/env python3
"""Run optional seeded RTL regressions with cocotb and an installed simulator."""

import argparse
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SUITES = {
    "standalone": {
        "top": "verification_top",
        "module": "test_datapath",
        "sources": [
            "feed_line_arbitrator.sv", "nasdaq_itch_moldudp64.sv", "cme_sbe_parser.sv",
            "hft_network_stack.sv", "nexus_network_stack_top.sv",
        ],
        "tests": {"arbiter_cycle_model", "axi_whole_frame_integrity",
                  "parser_wire_models", "loopback_reset_contract"},
    },
    "ingress": {
        "top": "nexus_mold_ingress",
        "module": "test_ingress",
        "sources": [
            "market_data/moldudp64_packet_buffer.sv",
            "market_data/itch_a_normalizer.sv",
            "mold_session_gate.sv", "nexus_mold_ingress.sv",
        ],
        "tests": {"mold_ingress_transactions"},
    },
}


def check_results(path, expected):
    cases = ET.parse(path).getroot().findall(".//testcase")
    if len(cases) != len(expected) or {case.get("name") for case in cases} != expected:
        raise RuntimeError(f"Incomplete cocotb result set: {path}")
    if any(case.find(tag) is not None for case in cases for tag in ("failure", "error", "skipped")):
        raise RuntimeError(f"Failed or skipped cocotb test: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", choices=("icarus", "verilator"), default="icarus")
    parser.add_argument("--suite", choices=("all", *SUITES), default="all")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-rtl-cocotb")
    parser.add_argument("--seed", action="append", type=int,
                        help="repeat for additional reproducible runs")
    args = parser.parse_args()
    if sys.flags.optimize or os.environ.get("PYTHONOPTIMIZE", "0") not in ("", "0"):
        raise RuntimeError("Python optimisation must remain disabled for test assertions")
    from cocotb_tools.runner import get_runner

    build_dir = args.build_dir.expanduser().resolve()
    build_args = ["-g2012"] if args.sim == "icarus" else []
    if args.sim == "icarus" and os.environ.get("IVERILOG_BASE"):
        base = Path(os.environ["IVERILOG_BASE"]).expanduser().resolve()
        if not (base / "ivl").is_file() or not (base / "ivlpp").is_file():
            raise RuntimeError(f"Invalid relocated Icarus component directory: {base}")
        build_args += ["-B", str(base)]
    sys.path.insert(0, str(HERE))
    for name in SUITES if args.suite == "all" else (args.suite,):
        suite = SUITES[name]
        output = build_dir / name
        output.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "src/nexus/fpga" / source for source in suite["sources"]]
        if name == "standalone":
            sources.append(HERE / "verification_top.sv")
        runner = get_runner(args.sim)
        runner.build(
            sources=sources,
            hdl_toplevel=suite["top"],
            build_dir=output / "compile",
            build_args=build_args,
            timescale=("1ns", "1ps"),
            always=True,
            log_file=output / "build.log",
        )
        for seed in args.seed or [20261011, 0x5EED1234]:
            result_dir = output / f"seed-{seed}"
            result_dir.mkdir(parents=True, exist_ok=True)
            result_file = result_dir / "results.xml"
            # A stale result must never make an incomplete simulation appear successful.
            result_file.unlink(missing_ok=True)
            print(f"Running {name} with {args.sim}, seed {seed}", flush=True)
            runner.test(
                hdl_toplevel=suite["top"],
                test_module=suite["module"],
                test_dir=result_dir,
                results_xml=str(result_file),
                seed=seed,
                extra_env={"HFT_RTL_SEED": str(seed)},
                log_file=result_dir / "simulation.log",
            )
            check_results(result_file, suite["tests"])
            print(f"Passed {len(suite['tests'])} tests for {name}, seed {seed}", flush=True)


if __name__ == "__main__":
    main()
