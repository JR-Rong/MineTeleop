from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .config import VehicleConfig


@dataclass(frozen=True)
class PreflightCheck:
    name: str
    status: str
    path: str
    message: str


@dataclass(frozen=True)
class PreflightReport:
    checks: tuple[PreflightCheck, ...]

    @property
    def ready(self) -> bool:
        return all(check.status in {"ready", "skipped"} for check in self.checks)


class VehiclePreflightChecker:
    def __init__(self, config: VehicleConfig, hardware_devices: Iterable[Path | str] = ()) -> None:
        self.config = config
        configured_devices = tuple(hardware_devices) or tuple(config.hardware.preflight_devices)
        self.hardware_devices = tuple(Path(path) for path in configured_devices)

    def run(self) -> PreflightReport:
        checks: list[PreflightCheck] = []
        for camera in self.config.enabled_cameras:
            name = f"camera.{camera.camera_id}.device"
            if camera.device == "testsrc":
                checks.append(PreflightCheck(name, "skipped", camera.device, "test source does not require a device"))
            else:
                checks.append(self._check_readable_path(name, Path(camera.device)))
        checks.append(self._check_writable_directory("recording.root_dir", Path(self.config.recording.root_dir)))
        for device in self.hardware_devices:
            checks.append(self._check_readable_path(f"hardware.{device}", device))
        return PreflightReport(tuple(checks))

    def _check_readable_path(self, name: str, path: Path) -> PreflightCheck:
        if not path.exists():
            return PreflightCheck(name, "missing", str(path), f"{path} is missing")
        if not os.access(path, os.R_OK):
            return PreflightCheck(name, "not_readable", str(path), f"{path} is not readable")
        return PreflightCheck(name, "ready", str(path), f"{path} is readable")

    def _check_writable_directory(self, name: str, path: Path) -> PreflightCheck:
        if not path.exists():
            return PreflightCheck(name, "missing", str(path), f"{path} is missing")
        if not path.is_dir():
            return PreflightCheck(name, "not_directory", str(path), f"{path} is not a directory")
        if not os.access(path, os.W_OK):
            return PreflightCheck(name, "not_writable", str(path), f"{path} is not writable")
        return PreflightCheck(name, "ready", str(path), f"{path} is writable")
