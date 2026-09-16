"""Gate repeated kernel measurements against an empirical same-image baseline."""

import argparse
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.kernel_validation import BENCHMARKS, comparison, load_json, validate_report


def gate(baselines: list[dict], candidates: list[dict], max_noise: float | None) -> dict:
    """Require five calibration guests and three confirmation guests per scenario.

    Noise is the full range of baseline run medians relative to its minimum.
    The upper acceptance bound allows this observed multiplicative variation
    above the slowest baseline median. This is an empirical engineering rule,
    not a population confidence interval or a native-hardware performance claim.
    """
    result = dict(status="incomplete", rule="empirical-range-v1", scenarios=[])
    try:
        if max_noise is not None and (not math.isfinite(max_noise) or not 0 <= max_noise < 1):
            raise ValueError("maximum acceptable noise ratio must be finite and in [0, 1)")
        result["max_noise"] = max_noise
        if len(baselines) < 5 or len(candidates) < 3:
            raise ValueError("at least five baseline and three candidate reports are required")
        seen = set()
        for group in (baselines, candidates):
            identity = None
            for report in group:
                validate_report(report)
                provenance = report.get("provenance", {})
                keys = ("revision", "source_sha256", "image_sha256", "fixture_sha256")
                current = tuple(provenance.get(key) for key in keys)
                if provenance.get("source_status") != "recorded" or any(
                    not isinstance(value, str)
                    or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}" if key == "revision" else r"[0-9a-f]{64}", value)
                    for key, value in zip(keys, current, strict=True)
                ):
                    raise ValueError("recorded source and image provenance is required")
                environment = report["comparison_environment"]
                if any(
                    key not in environment
                    for key in (
                        "arch",
                        "build",
                        "qemu",
                        "host",
                        "settings",
                        "accelerator",
                        "clock_policy",
                        "fixture_sha256",
                        "dtb_sha256",
                        "machine",
                        "cpu_model",
                    )
                ) or not all(environment[key] for key in ("qemu", "host", "machine", "cpu_model")):
                    raise ValueError("complete comparison environment is required")
                if environment["build"].get("type") != "Release":
                    raise ValueError("performance acceptance requires Release measurements")
                if not environment["build"].get("compiler") or not isinstance(environment["build"].get("flags"), dict):
                    raise ValueError("compiler identity and build flags are required")
                if any(not isinstance(environment["build"]["flags"].get(key), str) for key in ("compile", "link")):
                    raise ValueError("compile and link flags are required")
                if environment["fixture_sha256"] != provenance["fixture_sha256"]:
                    raise ValueError("fixture identity disagrees with the comparison environment")
                if identity is not None and current != identity:
                    raise ValueError("each repeat group must use the same source, image and fixture")
                identity = current
                # Re-labelling one saved run must not manufacture repetitions.
                for guest in report["guests"]:
                    parameters = guest.get("parameters")
                    if (
                        not isinstance(parameters, dict)
                        or type(parameters.get("order")) is not int
                        or not 0 <= parameters["order"] <= 4
                    ):
                        raise ValueError("workload parameters are required")
                    raw = {k: guest.get(k) for k in ("workload", "clock", "calibration", "batches")}
                    digest = hashlib.sha256(json.dumps(raw, sort_keys=True).encode()).hexdigest()
                    if digest in seen:
                        raise ValueError("duplicate scenario measurements cannot count as independent repeats")
                    seen.add(digest)
                if report["requested"] != BENCHMARKS or report["not_run"]:
                    raise ValueError("every report must complete the full benchmark catalog in order")

        base = baselines[0]
        compared = [comparison(base, report) for report in baselines + candidates]
        for index, name in enumerate(BENCHMARKS):
            records = [items[index] for items in compared]
            item = dict(workload=name, status="incomplete")
            result["scenarios"].append(item)
            if any(row["status"] != "comparable" for row in records):
                item["reason"] = "invalid or incompatible measurements"
                continue
            values = [row["current_median_ns"] for row in records]
            before, after = values[: len(baselines)], values[len(baselines) :]
            noise = max(before) / min(before) - 1
            candidate_noise = max(after) / min(after) - 1
            upper = max(before) * (1 + noise)
            if not all(math.isfinite(value) for value in (noise, candidate_noise, upper)):
                item["reason"] = "non-finite calibration range"
                continue
            item.update(
                baseline_run_medians_ns=before,
                candidate_run_medians_ns=after,
                calibration_noise=noise,
                candidate_noise=candidate_noise,
                upper_bound_ns=upper,
                regression_ratio=upper / statistics.median(before) - 1,
            )
            if max_noise is None:
                item["reason"] = "maximum acceptable noise ratio has not been supplied"
            elif noise > max_noise or candidate_noise > max_noise:
                item["reason"] = "excessive noise"
            elif min(after) > upper:
                item["status"] = "regression"
            elif max(after) <= upper:
                item["status"] = "passed"
            else:
                item["reason"] = "repeats disagree across the calibrated boundary"
        statuses = [item["status"] for item in result["scenarios"]]
        result["status"] = (
            "regression"
            if "regression" in statuses
            else "passed"
            if all(s == "passed" for s in statuses)
            else "incomplete"
        )
    except (ValueError, KeyError, TypeError, AttributeError, IndexError, OverflowError) as error:
        result["reason"] = str(error)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, action="append", required=True)
    parser.add_argument("--current", type=Path, action="append", required=True)
    parser.add_argument(
        "--max-noise", type=float, help="explicit accepted run-to-run noise ratio, not a regression threshold"
    )
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    # Never overwrite earlier calibration or acceptance evidence.
    with options.output.open("x") as stream:
        groups, evidence = [], []
        try:
            for paths in (options.baseline, options.current):
                reports = []
                for path in paths:
                    raw = path.read_bytes()
                    reports.append(load_json(raw.decode()))
                    evidence.append(dict(path=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest()))
                groups.append(reports)
            result = gate(*groups, options.max_noise)
        except (OSError, ValueError, UnicodeError) as error:
            result = dict(status="incomplete", reason=str(error))
        result["reports"] = evidence
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(f"{result['status']}: {options.output}")
    return {"passed": 0, "regression": 1, "incomplete": 2}[result["status"]]


if __name__ == "__main__":
    try:
        exit_code = main()
    except (OSError, ValueError) as error:
        print(f"incomplete: {error}", file=sys.stderr)
        exit_code = 2
    raise SystemExit(exit_code)
