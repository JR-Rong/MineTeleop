#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.deployment_validation import TargetHostValidationArchive  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize archived target-host validation results.")
    parser.add_argument("--results", required=True, help="Path to target_host_validation_results.jsonl.")
    parser.add_argument(
        "--verify-artifacts",
        action="store_true",
        help="Fail the report when referenced stdout/stderr log files are missing.",
    )
    args = parser.parse_args()

    archive = TargetHostValidationArchive.from_jsonl(args.results)
    for line in archive.to_jsonl(verify_artifacts=args.verify_artifacts):
        print(line)
    return 0 if archive.passed_for_report(verify_artifacts=args.verify_artifacts) else 2


if __name__ == "__main__":
    raise SystemExit(main())
