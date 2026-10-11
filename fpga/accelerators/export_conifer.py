#!/usr/bin/env python3
"""Write an optional fixed-point HLS tree kernel from a trusted XGBoost model."""

import argparse
import hashlib
import importlib.metadata
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CONIFER_COMMIT = "29b60872bfa94d182637280355c1d993b8742050"
DEFAULT_SOURCE = ROOT / ".deps" / "fpga" / "conifer"


def checked_precision(value):
    # Restrict emitted C++ tokens and make rounding and overflow policy explicit.
    match = re.fullmatch(
        r"ap_fixed<([0-9]+),([0-9]+),(AP_TRN|AP_RND_CONV),(AP_WRAP|AP_SAT)>", value
    )
    if not match:
        raise argparse.ArgumentTypeError(
            "Use ap_fixed<W,I,AP_TRN|AP_RND_CONV,AP_WRAP|AP_SAT> without spaces"
        )
    width, integer_bits = int(match[1]), int(match[2])
    if not 2 <= integer_bits <= width <= 64:
        raise argparse.ArgumentTypeError("Expected 2 <= I <= W <= 64")
    return value


def checked_token(value):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", value):
        raise argparse.ArgumentTypeError("Expected a device part containing letters, digits, '_' or '-'")
    return value


def git_read(source, *arguments):
    return subprocess.run(
        ["git", "-C", str(source), *arguments], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout.strip()


def checked_source(source):
    source = source.expanduser().resolve()
    if not (source / ".git").is_dir():
        raise ValueError("Conifer must be a source checkout fetched by scripts/fpga_deps.py")
    if git_read(source, "rev-parse", "HEAD") != CONIFER_COMMIT:
        raise ValueError("Conifer checkout does not match the adapter's pinned revision")
    if git_read(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Conifer checkout contains local changes")
    return source


def describe_model(document, runtime_version):
    """Reject model structures whose semantics the pinned converter cannot retain."""
    if document.get("version") != [int(part) for part in runtime_version.split(".")]:
        raise ValueError("Saved-model version must exactly match the declared XGBoost runtime")
    learner = document["learner"]
    parameters = learner["learner_model_param"]
    booster = learner["gradient_booster"]
    objective = learner["objective"]["name"]
    if booster.get("name") != "gbtree":
        raise ValueError("Only gbtree models are supported; DART and linear models are excluded")
    if objective not in ("binary:logistic", "reg:squarederror"):
        raise ValueError("Supported objectives are binary:logistic and reg:squarederror")
    if int(parameters["num_class"]) != 0 or int(parameters.get("num_target", "1")) != 1:
        raise ValueError("Only one-output binary classification or regression is supported")
    if "best_iteration" in learner.get("attributes", {}):
        raise ValueError("Freeze the intended tree subset and remove early-stopping attributes before export")

    model = booster["model"]
    trees = model["trees"]
    if not trees or int(model["gbtree_model_param"]["num_trees"]) != len(trees):
        raise ValueError("Expected a nonempty, complete tree ensemble")
    if int(model["gbtree_model_param"].get("num_parallel_tree", "1")) != 1:
        raise ValueError("Parallel-tree forest models are outside this export contract")
    if len(model["tree_info"]) != len(trees) or any(int(group) != 0 for group in model["tree_info"]):
        raise ValueError("Expected one output group for all trees")
    for tree in trees:
        if any(int(kind) != 0 for kind in tree.get("split_type", [])):
            raise ValueError("Categorical splits are not supported")
        if tree.get("categories") or tree.get("categories_nodes"):
            raise ValueError("Categorical splits are not supported")
        if int(tree["tree_param"].get("size_leaf_vector", "0")) not in (0, 1):
            raise ValueError("Vector-valued leaves are not supported")
        if not all(math.isfinite(float(value)) for value in tree["split_conditions"]):
            raise ValueError("Tree thresholds and leaf values must be finite")
        if not all(math.isfinite(float(value)) for value in tree.get("loss_changes", [])):
            raise ValueError("Tree gains must be finite for generated parameter literals")

    n_features = int(parameters["num_feature"])
    if n_features < 1:
        raise ValueError("Expected at least one feature")
    features = learner.get("feature_names") or [f"f{index}" for index in range(n_features)]
    if len(features) != n_features or len(set(features)) != n_features:
        raise ValueError("Feature names must be unique and match num_feature")
    if any(kind == "c" for kind in learner.get("feature_types", [])):
        raise ValueError("Categorical input features are not supported")

    base = json.loads(parameters["base_score"])
    if isinstance(base, list):
        if len(base) != 1:
            raise ValueError("Expected one scalar base score")
        base = base[0]
    base = float(base)
    if not math.isfinite(base):
        raise ValueError("Base score must be finite")
    if objective == "binary:logistic":
        if not 0 < base < 1:
            raise ValueError("Binary logistic base score must be strictly between zero and one")
        raw_base = math.log(base / (1 - base))
    else:
        raw_base = base
    # This revision's pre-2.0 converter always substitutes zero for the base score.
    if int(runtime_version.split(".")[0]) < 2 and raw_base != 0:
        raise ValueError("Conifer requires a zero raw base score for XGBoost before 2.0")
    return {
        "objective": objective,
        "features_in_order": features,
        "n_trees": len(trees),
        "raw_base_score": raw_base,
        "output_semantics": "raw_margin" if objective == "binary:logistic" else "regression_score",
    }


def finish_hls_project(output, project_name):
    # The pinned writer's declaration has an obsolete tree_scores argument;
    # its implemented top and BDT::decision_function both take two arguments.
    header = output / "firmware" / f"{project_name}.h"
    declaration = (
        "\tinput_arr_t data,\n\tscore_arr_t score,\n"
        "\tscore_t tree_scores[BDT::fn_classes(n_classes) * n_trees]);"
    )
    text = header.read_text(encoding="utf-8")
    if text.count(declaration) != 1:
        raise ValueError("Pinned HLS declaration anchor changed; inspect the writer before exporting")
    header.write_text(text.replace(declaration, "\tinput_arr_t data,\n\tscore_arr_t score);"), encoding="utf-8")

    # The upstream bench still calls the obsolete signature and reports a tree
    # output that the top does not supply. Do not ship it as a validation harness.
    build_script = output / "build_hls.tcl"
    text = build_script.read_text(encoding="utf-8")
    bench_lines = (
        'add_files -tb ${prj_name}_test.cpp -cflags "-I firmware/ -std=c++0x"\n',
        "add_files -tb tb_data\n",
    )
    for line in bench_lines:
        if text.count(line) != 1:
            raise ValueError("Pinned HLS build anchor changed; inspect the writer before exporting")
        text = text.replace(line, "")
    build_script.write_text(text, encoding="utf-8")
    (output / f"{project_name}_test.cpp").unlink()
    (output / "build_opt.tcl").write_text(
        "# Numerical validation requires a separate model-specific harness.\n"
        "set opt(csim) 0\nset opt(cosim) 0\n", encoding="utf-8"
    )


def export(args):
    source = checked_source(args.conifer_source)
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.xgboost_version):
        raise ValueError("Declare an exact stable XGBoost version, for example 1.7.6")
    if not math.isfinite(args.clock_ns) or args.clock_ns <= 0:
        raise ValueError("Clock period must be finite and positive")
    if not re.fullmatch(r"nexus_[A-Za-z][A-Za-z0-9_]*", args.project_name) or "__" in args.project_name:
        raise ValueError("Project name must begin with nexus_ and use an identifier suffix without '__'")
    output = args.output.expanduser().resolve()
    if output == source or source in output.parents:
        raise ValueError("Output must not be inside the pinned Conifer checkout")
    model_path = args.model.expanduser().resolve()
    if model_path.suffix.lower() != ".json":
        raise ValueError("Provide Booster.save_model JSON, not a pickle, dump or UBJSON file")
    model_bytes = model_path.read_bytes()
    description = describe_model(json.loads(model_bytes), args.xgboost_version)
    versions = {
        package: importlib.metadata.version(package)
        for package in ("xgboost", "numpy", "pandas", "scipy", "packaging", "pybind11")
    }
    if versions["xgboost"] != args.xgboost_version:
        raise ValueError("Installed XGBoost version differs from --xgboost-version")

    # Import only the inspected checkout. Do not install packages or load a wheel
    # that happens to have the same public version as a different source revision.
    if "conifer" in sys.modules:
        raise ValueError("Run the exporter in a fresh Python process")
    sys.path.insert(0, str(source))
    import conifer
    import xgboost

    if Path(conifer.__file__).resolve() != source / "conifer" / "__init__.py":
        raise ValueError("Imported Conifer does not belong to the pinned source checkout")
    # The pinned backend initializer imports absent driver names and PYNQ even
    # for source export. Load its writer directly without that runtime package.
    writer_spec = importlib.util.spec_from_file_location(
        "nexus_conifer_hls_writer", source / "conifer" / "backends" / "xilinxhls" / "writer.py"
    )
    if writer_spec is None or writer_spec.loader is None:
        raise ValueError("Cannot load the pinned Conifer HLS writer")
    writer = importlib.util.module_from_spec(writer_spec)
    writer_spec.loader.exec_module(writer)
    booster = xgboost.Booster()
    booster.load_model(bytearray(model_bytes))
    config = writer.auto_config()
    config.update({
        "ProjectName": args.project_name,
        "OutputDir": str(output),
        "Precision": args.precision,
        "XilinxPart": args.part,
        "ClockPeriod": args.clock_ns,
        "Unroll": True,
        "AcceleratorConfig": None,
    })
    ensemble = conifer.converters.get_converter("xgboost").convert(booster)
    model = writer.make_model(ensemble, config)
    if model.n_trees != description["n_trees"] or model.n_features != len(description["features_in_order"]):
        raise ValueError("Converter changed the declared model dimensions")
    if len(model.init_predict) != 1 or not math.isclose(
        float(model.init_predict[0]), description["raw_base_score"], rel_tol=1e-12, abs_tol=1e-12
    ):
        raise ValueError("Converter did not preserve the raw base score")

    # Claim a fresh directory before any upstream writer can overwrite files.
    output.mkdir(parents=True, exist_ok=False)
    incomplete = output / "EXPORT_INCOMPLETE"
    incomplete.write_text("Source generation did not finish. Do not integrate this directory.\n", encoding="utf-8")
    model.write()
    finish_hls_project(output, args.project_name)
    shutil.copyfile(source / "LICENSE", output / "LICENSE.conifer")
    metadata = {
        "schema": 1,
        "source_only": True,
        "numerical_validation_performed": False,
        "synthesis_performed": False,
        "conifer_commit": CONIFER_COMMIT,
        "conifer_reported_version": str(conifer.__version__),
        "conifer_source": str(source),
        "model_sha256": hashlib.sha256(model_bytes).hexdigest(),
        "python": sys.version,
        "package_versions": versions,
        "precision": args.precision,
        "part": args.part,
        "clock_ns_requested": args.clock_ns,
        "project_name": args.project_name,
        "input_contract": "Dense numeric features in declared order; reject missing and nonfinite values upstream",
        "valid_score_indices": [0],
        "writer_adjustments": [
            "direct_writer_import_without_board_runtime",
            "two_argument_top_declaration",
            "obsolete_upstream_bench_omitted",
        ],
        **description,
    }
    (output / "export_metadata.json").write_text(
        json.dumps(metadata, indent=2, allow_nan=False) + "\n", encoding="utf-8"
    )
    incomplete.unlink()
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True, help="Trusted Booster.save_model JSON")
    parser.add_argument("--output", type=Path, required=True, help="New output directory; existing paths are refused")
    parser.add_argument("--precision", type=checked_precision, required=True)
    parser.add_argument("--part", type=checked_token, required=True, help="Exact part from the licensed board project")
    parser.add_argument("--clock-ns", type=float, required=True, help="Requested HLS clock period; not a timing guarantee")
    parser.add_argument("--xgboost-version", required=True, help="Exact runtime and saved-model version")
    parser.add_argument("--project-name", default="nexus_tree_score")
    parser.add_argument("--conifer-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument(
        "--dense-finite-features", action="store_true", required=True,
        help="Declare that the enclosing feature pipeline rejects missing and nonfinite values",
    )
    args = parser.parse_args()
    try:
        output = export(args)
    except (OSError, ValueError, KeyError, TypeError, AssertionError, ImportError,
            subprocess.CalledProcessError) as error:
        parser.exit(1, f"conifer export: {error}\n")
    print(f"HLS sources written to {output}; no compilation, inference or synthesis was run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
