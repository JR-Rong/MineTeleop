#!/usr/bin/env python3
"""Summarize transient hold events from exported vehicle runtime logs (JSONL).

Offline only: reads the given files, never contacts a vehicle or service.
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def summarize(paths):
    entered = resumed = escalated = hard_stops = ignored = 0
    reasons = Counter()
    causes = Counter()
    durations = []
    for path in paths:
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                ignored += 1
                continue
            name = event.get("event") if isinstance(event, dict) else None
            last = event.get("last") if isinstance(event, dict) and isinstance(event.get("last"), dict) else {}
            if name == "vehicle_transient_hold_entered":
                entered += 1
                causes.update(cause for cause in event.get("causes", []) if isinstance(cause, str))
            elif name == "vehicle_transient_hold_resumed":
                resumed += 1
                if isinstance(last.get("duration_ms"), (int, float)):
                    durations.append(last["duration_ms"])
            elif name == "vehicle_transient_hold_escalated":
                escalated += 1
                reasons[last.get("escalation_reason") or "unknown"] += 1
            elif name == "control_hard_stop_transition":
                hard_stops += 1
    return {
        "holds_entered": entered, "resumed": resumed, "escalated": escalated, "hard_stops": hard_stops,
        "causes": dict(causes), "escalation_reasons": dict(reasons), "ignored_lines": ignored,
        "resumed_duration_ms": {"count": len(durations), "p50": percentile(durations, 0.5),
                                "p95": percentile(durations, 0.95), "max": max(durations) if durations else None},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="exported vehicle runtime JSONL files")
    arguments = parser.parse_args()
    json.dump(summarize(arguments.logs), sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
