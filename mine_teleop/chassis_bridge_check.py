from __future__ import annotations

import json
import shlex
import shutil
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_CHASSIS_CONTROL_ROOT = Path("/Volumes/SystemDisk/Workspace/ChassisControl")
DEFAULT_MINEPILOT_ROOT = Path("/Volumes/SystemDisk/Workspace/MinePilot")
DEFAULT_CHASSIS_CONTROL_BRANCH = "UI_Test"
DEFAULT_MINEPILOT_BRANCH = "merge_ui_test"
DEFAULT_DOCKER_IMAGE = "minepilot-build-env"
DEFAULT_DOCKER_PLATFORM = "linux/amd64"
DEFAULT_CONTAINER_WORKSPACE = "/workspace"
REQUIRED_CHASSIS_CONTROL_SYMBOLS = (
    "Initialize(",
    "UpdateVehicleState(",
    "RunArmingStateMachine(",
    "ResetArmingStateMachine(",
    "ResetDisarmSequence(",
    "SendCanMessage(",
    "EmergencyStopWheels()",
)


@dataclass(frozen=True)
class ChassisBridgeCheck:
    name: str
    status: str
    path: str
    message: str


@dataclass(frozen=True)
class ChassisBridgeCheckReport:
    checks: tuple[ChassisBridgeCheck, ...]

    @property
    def ready(self) -> bool:
        return all(check.status in {"ready", "skipped"} for check in self.checks)

    def to_jsonl(self) -> tuple[str, ...]:
        summary = {
            "event": "chassis_bridge_check",
            "ready": self.ready,
            "check_count": len(self.checks),
        }
        lines = [json.dumps(summary, ensure_ascii=False, sort_keys=True)]
        lines.extend(json.dumps(asdict(check), ensure_ascii=False, sort_keys=True) for check in self.checks)
        return tuple(lines)


@dataclass(frozen=True)
class ChassisBridgeDockerCommandPlan:
    image: str
    platform: str
    build_image_command: str
    run_command: str

    def to_jsonl(self) -> tuple[str, ...]:
        record = asdict(self)
        record["event"] = "chassis_bridge_docker_command"
        return (json.dumps(record, ensure_ascii=False, sort_keys=True),)


def build_chassis_bridge_docker_command_plan(
    *,
    host_repo_root: Path | str,
    chassis_control_root: Path | str = DEFAULT_CHASSIS_CONTROL_ROOT,
    minepilot_root: Path | str = DEFAULT_MINEPILOT_ROOT,
    chassis_control_library: Path | str | None = None,
    build_dir: Path | str = "build/chassis-control-bridge-check",
    build_target: str = "mine_teleop_chassis_bridge",
    image: str = DEFAULT_DOCKER_IMAGE,
    platform: str = DEFAULT_DOCKER_PLATFORM,
    container_workspace: str = DEFAULT_CONTAINER_WORKSPACE,
) -> ChassisBridgeDockerCommandPlan:
    host_repo_root = Path(host_repo_root)
    chassis_control_root = Path(chassis_control_root)
    minepilot_root = Path(minepilot_root)
    container_repo_root = f"{container_workspace}/mine-teleop"
    container_chassis_root = f"{container_workspace}/ChassisControl"
    container_minepilot_root = f"{container_workspace}/MinePilot"
    container_build_dir = _container_build_dir(container_repo_root, build_dir)
    container_source_dir = f"{container_repo_root}/deployments/chassis-control-bridge"
    cmake_configure_args = [
        "cmake",
        "-S",
        _quote(container_source_dir),
        "-B",
        _quote(container_build_dir),
        f"-DCHASSIS_CONTROL_ROOT={_quote(container_chassis_root)}",
        f"-DMINEPILOT_ROOT={_quote(container_minepilot_root)}",
    ]
    if chassis_control_library is not None:
        container_library = _container_path_for_mount(
            chassis_control_library,
            (
                (host_repo_root, container_repo_root),
                (chassis_control_root, container_chassis_root),
                (minepilot_root, container_minepilot_root),
            ),
        )
        cmake_configure_args.append(f"-DCHASSIS_CONTROL_LIBRARY={_quote(container_library)}")
    cmake_command = " && ".join(
        (
            " ".join(cmake_configure_args),
            " ".join(
                (
                    "cmake",
                    "--build",
                    _quote(container_build_dir),
                    "--target",
                    _quote(build_target),
                )
            ),
        )
    )
    build_image_command = " ".join(
        (
            "docker",
            "build",
            "--platform",
            _quote(platform),
            "-t",
            _quote(image),
            _quote(str(minepilot_root)),
        )
    )
    run_command = " ".join(
        (
            "docker",
            "run",
            "--rm",
            "--platform",
            _quote(platform),
            "-v",
            _quote(f"{host_repo_root}:{container_repo_root}"),
            "-v",
            _quote(f"{chassis_control_root}:{container_chassis_root}"),
            "-v",
            _quote(f"{minepilot_root}:{container_minepilot_root}"),
            "-w",
            _quote(container_repo_root),
            _quote(image),
            "bash",
            "-lc",
            _quote(cmake_command),
        )
    )
    return ChassisBridgeDockerCommandPlan(
        image=image,
        platform=platform,
        build_image_command=build_image_command,
        run_command=run_command,
    )


class ChassisBridgeChecker:
    def __init__(
        self,
        *,
        chassis_control_root: Path | str = DEFAULT_CHASSIS_CONTROL_ROOT,
        minepilot_root: Path | str = DEFAULT_MINEPILOT_ROOT,
        build_dir: Path | str = "build/chassis-control-bridge-check",
        source_dir: Path | str | None = None,
        chassis_control_library: Path | str | None = None,
        chassis_control_branch: str = DEFAULT_CHASSIS_CONTROL_BRANCH,
        minepilot_branch: str = DEFAULT_MINEPILOT_BRANCH,
        run_cmake: bool = True,
        run_build: bool = False,
        build_target: str = "mine_teleop_chassis_bridge",
        cmake_executable: str = "cmake",
    ) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        self.chassis_control_root = Path(chassis_control_root)
        self.minepilot_root = Path(minepilot_root)
        self.build_dir = Path(build_dir)
        self.source_dir = Path(source_dir) if source_dir is not None else repo_root / "deployments/chassis-control-bridge"
        self.chassis_control_library = Path(chassis_control_library) if chassis_control_library else None
        self.chassis_control_branch = chassis_control_branch
        self.minepilot_branch = minepilot_branch
        self.run_cmake = run_cmake
        self.run_build = run_build
        self.build_target = build_target
        self.cmake_executable = cmake_executable

    def run(self) -> ChassisBridgeCheckReport:
        checks = [
            self._check_directory("chassis_control.root", self.chassis_control_root),
            self._check_directory("minepilot.root", self.minepilot_root),
            self._check_git_branch("chassis_control.branch", self.chassis_control_root, self.chassis_control_branch),
            self._check_git_branch("minepilot.branch", self.minepilot_root, self.minepilot_branch),
            self._check_git_commit("chassis_control.commit", self.chassis_control_root),
            self._check_git_commit("minepilot.commit", self.minepilot_root),
            self._check_git_dirty("chassis_control.dirty", self.chassis_control_root),
            self._check_git_dirty("minepilot.dirty", self.minepilot_root),
            self._check_file("chassis_control.header", self.chassis_control_root / "chassis_control.h"),
            self._check_file(
                "chassis_control.can_common_header",
                self.chassis_control_root / "include/can/can_common.h",
            ),
            self._check_file("minepilot.chassis_control_header", self.minepilot_root / "chassis_control.h"),
            self._check_file("minepilot.can_common_header", self.minepilot_root / "include/can/can_common.h"),
            self._check_file("minepilot.can_message_header", self.minepilot_root / "include/can/can_message.h"),
            self._check_file("minepilot.can_db_header", self.minepilot_root / "include/can_db.h"),
            self._check_file("minepilot.can_receiver_header", self.minepilot_root / "include/can_receiver.h"),
            self._check_file("minepilot.can_sender_header", self.minepilot_root / "include/can_sender.h"),
            self._check_file("minepilot.can_db_source", self.minepilot_root / "src/can_db.cpp"),
            self._check_file("minepilot.can_receiver_source", self.minepilot_root / "src/can_receiver.cpp"),
            self._check_file("minepilot.can_sender_source", self.minepilot_root / "src/can_sender.cpp"),
        ]
        library_check = self._check_library()
        checks.extend((library_check, self._check_symbols(library_check)))
        checks.append(self._configure_cmake(tuple(checks)))
        if self.run_build:
            checks.append(self._build_cmake(tuple(checks)))
        return ChassisBridgeCheckReport(tuple(checks))

    def _check_directory(self, name: str, path: Path) -> ChassisBridgeCheck:
        if not path.exists():
            return ChassisBridgeCheck(name, "missing", str(path), f"{path} is missing")
        if not path.is_dir():
            return ChassisBridgeCheck(name, "not_directory", str(path), f"{path} is not a directory")
        return ChassisBridgeCheck(name, "ready", str(path), f"{path} is a directory")

    def _check_file(self, name: str, path: Path) -> ChassisBridgeCheck:
        if not path.exists():
            return ChassisBridgeCheck(name, "missing", str(path), f"{path} is missing")
        if not path.is_file():
            return ChassisBridgeCheck(name, "not_file", str(path), f"{path} is not a file")
        return ChassisBridgeCheck(name, "ready", str(path), f"{path} is a file")

    def _check_git_branch(self, name: str, path: Path, expected_branch: str) -> ChassisBridgeCheck:
        if not expected_branch:
            return ChassisBridgeCheck(name, "skipped", str(path), "Git branch check was skipped by request")
        if not path.is_dir():
            return ChassisBridgeCheck(name, "skipped", str(path), "Git branch check was skipped because the root is not ready")
        result = subprocess.run(
            ["git", "-C", str(path), "branch", "--show-current"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            return ChassisBridgeCheck(name, "skipped", str(path), "Git branch check was skipped because the root is not a git checkout")
        current_branch = result.stdout.strip()
        if current_branch == expected_branch:
            return ChassisBridgeCheck(name, "ready", str(path), f"{path} is on expected branch {expected_branch}")
        return ChassisBridgeCheck(
            name,
            "mismatch",
            str(path),
            f"{path} is on branch {current_branch or '<detached>'}; expected {expected_branch}",
        )

    def _check_git_commit(self, name: str, path: Path) -> ChassisBridgeCheck:
        if not path.is_dir():
            return ChassisBridgeCheck(name, "skipped", str(path), "Git commit check was skipped because the root is not ready")
        result = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            return ChassisBridgeCheck(name, "skipped", str(path), "Git commit check was skipped because HEAD is unavailable")
        commit = result.stdout.strip()
        return ChassisBridgeCheck(name, "ready", str(path), f"{path} HEAD commit={commit}")

    def _check_git_dirty(self, name: str, path: Path) -> ChassisBridgeCheck:
        if not path.is_dir():
            return ChassisBridgeCheck(name, "skipped", str(path), "Git dirty check was skipped because the root is not ready")
        result = subprocess.run(
            # --porcelain uses the stable short format with normal untracked-file
            # handling; avoid --untracked-files=all which can exhaust memory on
            # large source trees (ChassisControl/MinePilot).
            ["git", "-C", str(path), "status", "--porcelain"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            return ChassisBridgeCheck(name, "skipped", str(path), "Git dirty check was skipped because the root is not a git checkout")
        changed_count = len([line for line in result.stdout.splitlines() if line.strip()])
        dirty = "true" if changed_count else "false"
        return ChassisBridgeCheck(
            name,
            "ready",
            str(path),
            f"{path} dirty={dirty} changed_paths={changed_count}",
        )

    def _check_library(self) -> ChassisBridgeCheck:
        if self.chassis_control_library is not None:
            return self._check_file("chassis_control.library", self.chassis_control_library)

        candidates = tuple(self._library_candidates())
        for candidate in candidates:
            if candidate.is_file():
                return ChassisBridgeCheck(
                    "chassis_control.library",
                    "ready",
                    str(candidate),
                    f"{candidate} is the selected chassis_control library",
                )
        return ChassisBridgeCheck(
            "chassis_control.library",
            "missing",
            ", ".join(str(candidate) for candidate in candidates),
            "libchassis_control was not found in ChassisControl or MinePilot build locations",
        )

    def _library_candidates(self) -> Iterable[Path]:
        search_dirs = [
            self.chassis_control_root / "build/lib",
            self.chassis_control_root / "cmake-build-debug/lib",
            self.chassis_control_root / "cmake-build-release/lib",
            self.minepilot_root,
            self.minepilot_root / "build/lib",
            self.minepilot_root / "cmake-build-debug/lib",
            self.minepilot_root / "cmake-build-release/lib",
        ]
        names = [
            "libchassis_control.so",
            "libchassis_control.dylib",
            "libchassis_control.dll",
            "libchassis_control.a",
            "chassis_control.dll",
            "chassis_control.lib",
            "chassis_control.a",
        ]
        for directory in search_dirs:
            for name in names:
                yield directory / name

    def _check_symbols(self, library_check: ChassisBridgeCheck) -> ChassisBridgeCheck:
        if library_check.status != "ready":
            return ChassisBridgeCheck(
                "chassis_control.symbols",
                "skipped",
                library_check.path,
                "ChassisControl symbol check was skipped because the library is not ready",
            )
        nm = shutil.which("nm")
        if nm is None:
            return ChassisBridgeCheck(
                "chassis_control.symbols",
                "missing",
                "nm",
                "nm is not installed; cannot verify required ChassisControl symbols",
            )
        cxxfilt = shutil.which("c++filt")
        if cxxfilt is None:
            return ChassisBridgeCheck(
                "chassis_control.symbols",
                "missing",
                "c++filt",
                "c++filt is not installed; cannot demangle required ChassisControl symbols",
            )
        library_path = Path(library_check.path)
        symbol_output, failure_details = _read_symbol_output(nm, cxxfilt, library_path)
        if symbol_output is None:
            message = _failure_message(failure_details, default="Unable to read ChassisControl symbols")
            return ChassisBridgeCheck("chassis_control.symbols", "failed", str(library_path), message)
        missing = tuple(symbol for symbol in REQUIRED_CHASSIS_CONTROL_SYMBOLS if symbol not in symbol_output)
        if missing:
            return ChassisBridgeCheck(
                "chassis_control.symbols",
                "missing_symbol",
                str(library_path),
                f"{library_path} missing required ChassisControl symbols: {', '.join(missing)}",
            )
        return ChassisBridgeCheck(
            "chassis_control.symbols",
            "ready",
            str(library_path),
            f"{library_path} exports {len(REQUIRED_CHASSIS_CONTROL_SYMBOLS)} required ChassisControl symbols",
        )

    def _configure_cmake(self, previous_checks: tuple[ChassisBridgeCheck, ...]) -> ChassisBridgeCheck:
        if not self.run_cmake:
            return ChassisBridgeCheck(
                "cmake.configure",
                "skipped",
                str(self.build_dir),
                "CMake configure was skipped by request",
            )
        if any(check.status not in {"ready", "skipped"} for check in previous_checks):
            return ChassisBridgeCheck(
                "cmake.configure",
                "skipped",
                str(self.build_dir),
                "CMake configure was skipped because prerequisite checks failed",
            )

        cmake = shutil.which(self.cmake_executable)
        if cmake is None:
            return ChassisBridgeCheck("cmake.configure", "missing", self.cmake_executable, "cmake is not installed")

        command = [
            cmake,
            "-S",
            str(self.source_dir),
            "-B",
            str(self.build_dir),
            f"-DCHASSIS_CONTROL_ROOT={self.chassis_control_root}",
            f"-DMINEPILOT_ROOT={self.minepilot_root}",
        ]
        library_check = next(
            (check for check in previous_checks if check.name == "chassis_control.library" and check.status == "ready"),
            None,
        )
        if library_check is not None:
            command.append(f"-DCHASSIS_CONTROL_LIBRARY={library_check.path}")
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 0:
            return ChassisBridgeCheck(
                "cmake.configure",
                "ready",
                str(self.build_dir),
                "CMake configure completed for the chassis bridge",
            )
        details = (result.stderr or result.stdout).strip().splitlines()
        message = _failure_message(details, default="CMake configure failed")
        return ChassisBridgeCheck("cmake.configure", "failed", str(self.build_dir), message)

    def _build_cmake(self, previous_checks: tuple[ChassisBridgeCheck, ...]) -> ChassisBridgeCheck:
        configure = next((check for check in previous_checks if check.name == "cmake.configure"), None)
        if configure is not None and configure.status == "skipped" and not self.run_cmake:
            return ChassisBridgeCheck(
                "cmake.build",
                "failed",
                str(self.build_dir),
                "CMake build was requested but CMake configure was skipped",
            )
        if configure is None or configure.status != "ready":
            return ChassisBridgeCheck(
                "cmake.build",
                "skipped",
                str(self.build_dir),
                "CMake build was skipped because configure did not complete",
            )

        cmake = shutil.which(self.cmake_executable)
        if cmake is None:
            return ChassisBridgeCheck("cmake.build", "missing", self.cmake_executable, "cmake is not installed")

        result = subprocess.run(
            [
                cmake,
                "--build",
                str(self.build_dir),
                "--target",
                self.build_target,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode == 0:
            return ChassisBridgeCheck(
                "cmake.build",
                "ready",
                str(self.build_dir),
                f"CMake build completed for target {self.build_target}",
            )
        details = (result.stderr or result.stdout).strip().splitlines()
        message = _failure_message(details, default="CMake build failed")
        return ChassisBridgeCheck("cmake.build", "failed", str(self.build_dir), message)


def _read_symbol_output(nm: str, cxxfilt: str, library_path: Path) -> tuple[str | None, list[str]]:
    outputs: list[str] = []
    failure_details: list[str] = []
    for command in ([nm, "-D", str(library_path)], [nm, str(library_path)]):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 0:
            outputs.append(result.stdout)
        else:
            failure_details.extend((result.stderr or result.stdout).splitlines())
    if not outputs:
        return None, failure_details
    raw_symbols = "\n".join(outputs)
    demangle = subprocess.run(
        [cxxfilt],
        input=raw_symbols,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if demangle.returncode != 0:
        failure_details.extend((demangle.stderr or demangle.stdout).splitlines())
        return None, failure_details
    return demangle.stdout, failure_details


def _failure_message(details: list[str], *, default: str) -> str:
    for line in details:
        stripped = line.strip()
        if not stripped:
            continue
        lowered = stripped.lower()
        if "unsupported platform" in lowered:
            return (
                f"{stripped}; MinePilot CAN bridge sources require a Linux/Windows build host. "
                "Run this build gate on target Ubuntu or Docker linux/amd64."
            )
        if "error:" in lowered and "make:" not in lowered:
            return stripped
    for line in reversed(details):
        stripped = line.strip()
        if stripped:
            return stripped
    return default


def _container_build_dir(container_repo_root: str, build_dir: Path | str) -> str:
    build_path = Path(build_dir)
    if build_path.is_absolute():
        return str(build_path)
    return f"{container_repo_root}/{build_path.as_posix()}"


def _container_path_for_mount(path: Path | str, mounts: Iterable[tuple[Path | str, str]]) -> str:
    host_path = Path(path)
    for host_root, container_root in mounts:
        try:
            relative = host_path.relative_to(Path(host_root))
        except ValueError:
            continue
        relative_text = relative.as_posix()
        if relative_text == ".":
            return container_root
        return f"{container_root}/{relative_text}"
    return str(path)


def _quote(value: str) -> str:
    return shlex.quote(value)
