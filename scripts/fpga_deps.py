#!/usr/bin/env python3
"""Fetch pinned source dependencies without installing or executing upstream code."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
LOCK = REPOSITORY / "fpga" / "dependencies.lock.json"
DEFAULT_ROOT = REPOSITORY / ".deps" / "fpga"


def git(directory, *arguments, capture=False):
    environment = os.environ.copy()
    environment["GIT_TERMINAL_PROMPT"] = "0"
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else None


def read_lock():
    with LOCK.open(encoding="utf-8") as stream:
        lock = json.load(stream)
    if lock.get("schema") != 1:
        raise ValueError("Unsupported dependency lock schema")
    for name, entry in lock["dependencies"].items():
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError(f"Invalid dependency name: {name}")
        if not re.fullmatch(r"[0-9a-f]{40}", entry["commit"]):
            raise ValueError(f"Expected an immutable commit for {name}")
        if not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.git", entry["url"]):
            raise ValueError(f"Unsupported source URL for {name}")
        for path in entry.get("sparse_paths", []):
            if not path or path.startswith("/") or any(part in ("", ".", "..") for part in path.split("/")):
                raise ValueError(f"Invalid sparse path for {name}")
    return lock


def select(lock, names, profiles):
    selected = list(names)
    for profile in profiles:
        if profile == "all":
            selected.extend(lock["dependencies"])
        elif profile in lock["profiles"]:
            selected.extend(lock["profiles"][profile])
        else:
            raise ValueError(f"Unknown profile: {profile}")
    if not selected:
        selected.extend(lock["profiles"]["core"])
    for name in selected:
        if name not in lock["dependencies"]:
            raise ValueError(f"Unknown dependency: {name}")
    return list(dict.fromkeys(selected))


def inspect_checkout(directory, entry):
    # Refuse nested repositories, foreign origins and local source modifications.
    if not (directory / ".git").is_dir():
        raise ValueError(f"Not a managed source checkout: {directory}")
    if git(directory, "remote", "get-url", "origin", capture=True) != entry["url"]:
        raise ValueError(f"Unexpected origin: {directory}")
    if git(directory, "rev-parse", "HEAD", capture=True) != entry["commit"]:
        raise ValueError(f"Checkout revision differs from lock: {directory}")
    if git(directory, "status", "--porcelain", "--untracked-files=all", capture=True):
        raise ValueError(f"Dependency contains local changes: {directory}")


def fetch(root, name, entry):
    destination = root / name
    if destination.is_symlink():
        raise ValueError(f"Refusing a symbolic-link checkout: {destination}")
    if destination.exists():
        inspect_checkout(destination, entry)
        print(f"{name}: already at {entry['commit']}")
        return

    # Work in a unique sibling. Failure leaves existing source trees untouched.
    with tempfile.TemporaryDirectory(prefix=f".{name}-", dir=root) as temporary:
        staging = Path(temporary) / "checkout"
        staging.mkdir()
        git(staging, "init", "--quiet")
        git(staging, "remote", "add", "origin", entry["url"])
        git(staging, "-c", "protocol.version=2", "fetch", "--depth=1", "--filter=blob:none", "origin", entry["commit"])
        sparse = entry.get("sparse_paths", [])
        if sparse:
            git(staging, "sparse-checkout", "init", "--cone")
            git(staging, "sparse-checkout", "set", "--", *sparse)
        git(staging, "-c", "core.hooksPath=/dev/null", "checkout", "--quiet", "--detach", entry["commit"])
        inspect_checkout(staging, entry)
        if destination.exists():
            raise ValueError(f"Another fetch created {destination}; retry without overwriting it")
        staging.rename(destination)
    print(f"{name}: fetched {entry['commit']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("list", "fetch", "status"))
    parser.add_argument("names", nargs="*")
    parser.add_argument("--profile", action="append", default=[], help="core, dma, nic_references, accelerators, or all")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    lock = read_lock()
    selected = select(lock, args.names, args.profile)
    root = args.root.expanduser().resolve()

    if args.command == "fetch":
        root.mkdir(parents=True, exist_ok=True)
    for name in selected:
        entry = lock["dependencies"][name]
        if args.command == "list":
            print(f"{name}\n  commit: {entry['commit']}\n  license: {entry['license']}\n  role: {entry['role']}")
        elif args.command == "fetch":
            fetch(root, name, entry)
        else:
            inspect_checkout(root / name, entry)
            print(f"{name}: source revision matches lock")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"fpga_deps: {error}", file=sys.stderr)
        sys.exit(1)
