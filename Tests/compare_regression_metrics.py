#!/usr/bin/env python3
"""Compare two RealWorldRegression CSV files and fail on meaningful regressions."""
from __future__ import annotations
import argparse
import sys
import pandas as pd

DEFAULT_LIMITS = {
    "noiseFrameDelta": 0.0025,
    "wetFrameDelta": 0.0030,
    "highBandEnvelopeDeltaDb": 0.35,
    "outputPeak": 0.15,
    "blockInvarianceError": 2.0e-5,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--relative", type=float, default=0.20,
                        help="allowed relative increase for modulation metrics")
    args = parser.parse_args()

    baseline = pd.read_csv(args.baseline)
    candidate = pd.read_csv(args.candidate)
    keys = ["file", "mode"]
    merged = baseline.merge(candidate, on=keys, suffixes=("_base", "_new"))
    if len(merged) != len(candidate):
        print("Metric sets do not contain the same file/mode rows", file=sys.stderr)
        return 2

    failures: list[str] = []
    for _, row in merged.iterrows():
        label = f"{row['file']} mode {int(row['mode'])}"
        for metric in ("noiseFrameDelta", "wetFrameDelta", "highBandEnvelopeDeltaDb"):
            old = float(row[f"{metric}_base"])
            new = float(row[f"{metric}_new"])
            absolute = DEFAULT_LIMITS[metric]
            allowed = max(old * (1.0 + args.relative), old + absolute)
            if new > allowed:
                failures.append(f"{label}: {metric} {new:.6g} > {allowed:.6g}")
        if float(row["blockInvarianceError_new"]) > DEFAULT_LIMITS["blockInvarianceError"]:
            failures.append(f"{label}: block-size invariance failed")
        if int(row["finite_new"]) != 1:
            failures.append(f"{label}: non-finite output")
        if float(row["outputPeak_new"]) > max(4.0, float(row["outputPeak_base"]) + 0.15):
            failures.append(f"{label}: output peak regression")

    if failures:
        print("Regression comparison failed:")
        print("\n".join(f" - {failure}" for failure in failures))
        return 1

    print(f"Compared {len(merged)} file/mode rows: no significant regression.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
