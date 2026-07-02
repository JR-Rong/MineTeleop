from __future__ import annotations

import re
from dataclasses import dataclass

from .observability import ComponentLogEvent


@dataclass(frozen=True)
class TimeSyncStatus:
    source: str
    synchronized: bool
    offset_ms: float | None
    stratum: int | None

    @classmethod
    def from_chronyc_tracking(cls, output: str, source: str = "chrony") -> "TimeSyncStatus":
        stratum = None
        offset_ms = None
        synchronized = False
        for raw_line in output.splitlines():
            line = raw_line.strip()
            if line.startswith("Stratum"):
                stratum = int(line.split(":", 1)[1].strip())
            elif line.startswith("System time"):
                offset_ms = _parse_system_time_offset_ms(line)
            elif line.startswith("Leap status"):
                synchronized = line.split(":", 1)[1].strip().lower() == "normal"
        return cls(source=source, synchronized=synchronized, offset_ms=offset_ms, stratum=stratum)


@dataclass(frozen=True)
class TimeSyncAssessment:
    acceptable: bool
    log_event: ComponentLogEvent


class TimeSyncMonitor:
    def __init__(self, minimum: str, max_offset_ms: float = 100.0) -> None:
        if minimum not in {"ntp", "ptp"}:
            raise ValueError("minimum must be ntp or ptp")
        self.minimum = minimum
        self.max_offset_ms = max_offset_ms

    def assess(
        self,
        status: TimeSyncStatus,
        component: str,
        vehicle_id: str,
        session_id: str,
        now_ms: int,
    ) -> TimeSyncAssessment:
        offset_ms = status.offset_ms if status.offset_ms is not None else 0.0
        if not status.synchronized:
            level = "error"
            error_code = "time_not_synchronized"
            acceptable = False
        elif abs(offset_ms) > self.max_offset_ms:
            level = "warning"
            error_code = "time_offset_exceeds_threshold"
            acceptable = False
        else:
            level = "info"
            error_code = ""
            acceptable = True
        message = (
            f"time_sync source={status.source} minimum={self.minimum} "
            f"synchronized={status.synchronized} offset_ms={offset_ms:.3f} "
            f"stratum={status.stratum if status.stratum is not None else 'unknown'}"
        )
        return TimeSyncAssessment(
            acceptable=acceptable,
            log_event=ComponentLogEvent(
                ts_ms=now_ms,
                level=level,
                component=component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                event="time_sync_status",
                message=message,
                error_code=error_code,
            ),
        )


def _parse_system_time_offset_ms(line: str) -> float:
    match = re.search(r":\s*([0-9.]+)\s+seconds\s+(fast|slow)\s+of NTP time", line)
    if match is None:
        raise ValueError("chronyc System time line is not parseable")
    seconds = float(match.group(1))
    sign = 1 if match.group(2) == "fast" else -1
    return sign * seconds * 1000
