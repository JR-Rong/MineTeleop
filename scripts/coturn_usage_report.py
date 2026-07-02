#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.signaling_service import CoturnUsageLogParser, CoturnUsageSample  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit a redacted coturn usage acceptance report.")
    parser.add_argument("--log", required=True, help="Path to a coturn log file, or '-' for stdin.")
    args = parser.parse_args()

    lines = _read_lines(args.log)
    samples = _parse_samples(lines)
    relay_bytes_total = sum(sample.bytes_sent + sample.bytes_received for sample in samples)
    duration_ms_total = sum(sample.duration_ms for sample in samples)
    summary = {
        "event": "coturn_usage_report",
        "passed": bool(samples),
        "total_lines": len(lines),
        "parsed_samples": len(samples),
        "ignored_lines": len(lines) - len(samples),
        "session_count": len({sample.session_id for sample in samples}),
        "relay_bytes_total": relay_bytes_total,
        "duration_ms_total": duration_ms_total,
        "average_bitrate_kbps": _bitrate_kbps(relay_bytes_total, duration_ms_total),
    }
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    for sample in samples:
        print(json.dumps(_sample_record(sample), ensure_ascii=False, sort_keys=True))
    return 0 if samples else 2


def _read_lines(path: str) -> list[str]:
    if path == "-":
        return sys.stdin.read().splitlines()
    return Path(path).read_text(encoding="utf-8").splitlines()


def _parse_samples(lines: list[str]) -> list[CoturnUsageSample]:
    parser = CoturnUsageLogParser()
    samples: list[CoturnUsageSample] = []
    for line in lines:
        sample = parser.parse_line(line)
        if sample is not None:
            samples.append(sample)
    return samples


def _sample_record(sample: CoturnUsageSample) -> dict[str, object]:
    relay_bytes = sample.bytes_sent + sample.bytes_received
    return {
        "event": "coturn_usage_sample",
        "source": sample.source,
        "session_id": sample.session_id,
        "actor": sample.actor,
        "bytes_sent": sample.bytes_sent,
        "bytes_received": sample.bytes_received,
        "relay_bytes": relay_bytes,
        "duration_ms": sample.duration_ms,
        "bitrate_kbps": _bitrate_kbps(relay_bytes, sample.duration_ms),
    }


def _bitrate_kbps(relay_bytes: int, duration_ms: int) -> float:
    if duration_ms <= 0:
        return 0.0
    return round((relay_bytes * 8) / duration_ms, 3)


if __name__ == "__main__":
    raise SystemExit(main())
