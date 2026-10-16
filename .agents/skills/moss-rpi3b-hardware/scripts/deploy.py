#!/usr/bin/env python3
"""Run the checkout's Pi deploy CLI with a process-local JTAG speed ceiling."""

import argparse
import importlib.util
import os
import re
import sys
from pathlib import Path


def limit_speed(command, ceiling_khz):
    match = re.fullmatch(r"adapter speed ([0-9]+)", command)
    if match:
        # Never increase a lower speed already selected by the repository.
        return f"adapter speed {min(int(match[1]), ceiling_khz)}"
    return command


def find_repo(explicit):
    if explicit is not None:
        candidates = [explicit.resolve()]
    else:
        # A shared skill may live outside the checkout. Search only ancestors
        # of the working directory and this script; never scan another user's home.
        candidates = []
        for start in (Path.cwd(), Path(__file__).resolve().parent):
            candidates.extend((start, *start.parents))
    for root in candidates:
        if all((root / "scripts" / name).is_file() for name in ("rpi.py", "artifacts.py")):
            return root
    raise ValueError("Moss checkout not found; pass --repo with scripts/rpi.py and scripts/artifacts.py")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Pass repository deploy arguments after --; this command does not reset the board.",
    )
    parser.add_argument("--repo", type=Path, help="Moss checkout; otherwise discover from cwd or this skill")
    # Wire length, probe and target determine the stable rate; there is no
    # machine-independent speed default. The caller supplies a checked ceiling.
    parser.add_argument("--jtag-khz", type=int, required=True)
    parser.add_argument("deploy_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.jtag_khz <= 0:
        parser.error("--jtag-khz must be positive")
    if os.name != "posix":
        parser.error("The repository UART loader requires macOS/Linux; use a POSIX host with USB access")
    try:
        root = find_repo(args.repo)
    except ValueError as error:
        parser.error(str(error))
    source = root / "scripts/rpi.py"
    forwarded = args.deploy_args
    if forwarded[:1] == ["--"]:
        forwarded = forwarded[1:]
    if not forwarded:
        parser.error("Pass deploy arguments after -- (use -- --help to inspect them)")

    # Interpret relative manifests, DTBs and output paths consistently even
    # when invoked from another directory or from a globally installed skill.
    os.chdir(root)
    # Import the requested checkout rather than a stale installed 'scripts' package.
    # This separate process changes no repository source or running server until
    # the existing deployment itself issues its adapter-speed command.
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location("moss_rpi_skill_deploy", source)
    if spec is None or spec.loader is None:
        parser.error(f"Cannot load {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    original = module.OpenOCD.command

    def command(self, text):
        return original(self, limit_speed(text, args.jtag_khz))

    module.OpenOCD.command = command
    sys.argv = [str(source), "deploy", *forwarded]
    return module.main()


if __name__ == "__main__":
    raise SystemExit(main())
