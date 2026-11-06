#!/usr/bin/env python3
"""Correlate zero-limited native key observations with mock vehicle commands.

This checks input evidence only; path, video, clock and safety-stop acceptance
must be checked independently. Input and injected windows use the Windows clock.
"""
import argparse
import datetime as dt
import json
import re
from collections import Counter
from pathlib import Path

MASKS = {"forward": 1, "reverse": 2, "left": 4, "right": 8,
         "forward-left": 5, "forward-right": 9, "reverse-left": 6,
         "reverse-right": 10, "brake": 16}


def records(path):
    with path.open(encoding="utf-8-sig", errors="replace") as stream:
        for line in stream:
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    yield value
            except ValueError:
                continue


def timestamp(value):
    # PowerShell's round-trip format has seven fractional digits; Python 3.9
    # accepts at most six. Truncation preserves the millisecond window used here.
    normalized = re.sub(r"(\.\d{6})\d+(?=Z|[+-]\d\d:\d\d$)", r"\1", value)
    parsed = dt.datetime.fromisoformat(normalized.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("test window timestamps must include a timezone")
    return int(parsed.timestamp() * 1000)


def analyze(metadata, viewer_records, vehicle_records):
    inputs, sessions, dropped = [], set(), 0
    for batch in viewer_records:
        if batch.get("event") != "native_mock_input_batch":
            continue
        session = batch.get("session_id")
        if not isinstance(session, str) or not session:
            raise ValueError("input trace is missing session identity")
        sessions.add(session)
        dropped += batch.get("dropped", 0)
        inputs.extend(batch.get("records", []))
    if len(sessions) != 1 or dropped:
        raise ValueError("input trace has missing/multiple sessions or dropped records")
    session = next(iter(sessions))
    accepted = {}
    for batch in vehicle_records:
        for command in batch.get("commands", []):
            if (command.get("stage") == "receive_apply" and command.get("accepted") is True and
                    command.get("session_id") == session and isinstance(command.get("intent_seq"), int)):
                accepted.setdefault(command["intent_seq"], []).append(command)
    output = []
    for window in metadata.get("periodic_input", []):
        scenario = window["scenario"]
        if scenario not in MASKS:
            raise ValueError("unrecognized test scenario")
        start, end = timestamp(window["started_utc"]), timestamp(window["ended_utc"])
        if end <= start:
            raise ValueError("invalid test window")
        local = [entry for entry in inputs if entry.get("keys") == MASKS[scenario] and
                 start <= entry.get("wall_utc_ms", 0) <= end]
        matches = []
        revisions = {(entry.get("intent_seq"), entry.get("gear")) for entry in local}
        for intent_seq, gear in revisions:
            for command in accepted.get(intent_seq, []):
                if (command.get("requested_gear") == gear and
                        command.get("requested_throttle") == 0 and command.get("requested_steering") == 0 and
                        command.get("effective_throttle") == 0 and command.get("effective_steering") == 0 and
                        (scenario != "brake" or (command.get("requested_brake") or 0) > 0)):
                    matches.append(command)
        output.append({"scenario": scenario, "local_observations": len(local), "accepted_commands": len(matches),
                       "paths": dict(Counter(command.get("transport", "unknown") for command in matches))})
    coverage = dict(Counter(item["scenario"] for item in output if item["accepted_commands"]))
    return {"session_id": session, "scenario_details": output,
            "all_windows_matched": bool(output) and all(item["accepted_commands"] for item in output),
            "all_nine_scenarios_covered": set(coverage) == set(MASKS), "coverage": coverage,
            "scope": "mock input correlation only; not path, safety-stop, CAN or real-vehicle acceptance"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("viewer_log", type=Path)
    parser.add_argument("vehicle_log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with args.metadata.open(encoding="utf-8-sig") as stream:
        metadata = json.load(stream)
    summary = analyze(metadata, records(args.viewer_log), records(args.vehicle_log))
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if summary["all_windows_matched"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
