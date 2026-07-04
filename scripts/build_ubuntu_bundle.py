#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shlex
import shutil
import subprocess
import tempfile
import textwrap
from pathlib import Path


DEFAULT_CHASSIS_CONTROL_ROOT = Path("/Volumes/SystemDisk/Workspace/ChassisControl")
DEFAULT_MINEPILOT_ROOT = Path("/Volumes/SystemDisk/Workspace/MinePilot")
DEFAULT_CHASSIS_CONTROL_LIBRARY = DEFAULT_MINEPILOT_ROOT / "libchassis_control.so"


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a no-Docker-on-target Ubuntu bundle for Mine Teleop.")
    parser.add_argument("--output-dir", default="dist/mine-teleop-ubuntu-x86_64")
    parser.add_argument("--docker-workspace", default="")
    parser.add_argument("--docker-image", default="minepilot-build-env")
    parser.add_argument("--docker-platform", default="linux/amd64")
    parser.add_argument("--chassis-control-root", default=str(DEFAULT_CHASSIS_CONTROL_ROOT))
    parser.add_argument("--minepilot-root", default=str(DEFAULT_MINEPILOT_ROOT))
    parser.add_argument("--chassis-control-library", default=str(DEFAULT_CHASSIS_CONTROL_LIBRARY))
    parser.add_argument("--bridge-library", default="")
    parser.add_argument("--skip-bridge-build", action="store_true")
    parser.add_argument("--keep-workspace", action="store_true")
    parser.add_argument("--no-archive", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    output_dir = Path(args.output_dir).resolve()
    chassis_control_root = Path(args.chassis_control_root).resolve()
    minepilot_root = Path(args.minepilot_root).resolve()
    chassis_control_library = Path(args.chassis_control_library).resolve()
    bridge_library = Path(args.bridge_library).resolve() if args.bridge_library else None
    workspace = Path(args.docker_workspace).resolve() if args.docker_workspace else _default_workspace()

    container_library = _container_path_for(chassis_control_library, chassis_control_root, minepilot_root)
    container_bridge_library = (
        _container_path_for(bridge_library, chassis_control_root, minepilot_root)
        if bridge_library is not None
        else ""
    )
    container_script = _container_build_script(
        build_bridge=not args.skip_bridge_build,
        container_chassis_control_library=container_library,
        container_bridge_library=container_bridge_library,
    )
    docker_command = _docker_command_preview(
        workspace=workspace,
        image=args.docker_image,
        platform=args.docker_platform,
        container_script=container_script,
    )
    if args.dry_run:
        print(docker_command)
        return 0

    _prepare_workspace(
        workspace=workspace,
        repo_root=repo_root,
        chassis_control_root=chassis_control_root,
        minepilot_root=minepilot_root,
        chassis_control_library=chassis_control_library,
        bridge_library=bridge_library,
    )
    try:
        staged_output = _run_docker_build(
            workspace=workspace,
            image=args.docker_image,
            platform=args.docker_platform,
            container_script=container_script,
        )
        if output_dir.exists():
            shutil.rmtree(output_dir)
        shutil.copytree(staged_output, output_dir)
        archive_path = ""
        if not args.no_archive:
            archive_path = shutil.make_archive(str(output_dir), "gztar", output_dir.parent, output_dir.name)
        print(
            json.dumps(
                {
                    "event": "mine_teleop_ubuntu_bundle_built",
                    "output_dir": str(output_dir),
                    "archive_path": archive_path,
                    "executable": str(output_dir / "bin" / "mine-teleop"),
                    "library_dir": str(output_dir / "lib"),
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
    finally:
        if not args.keep_workspace:
            shutil.rmtree(workspace, ignore_errors=True)
    return 0


def _default_workspace() -> Path:
    shared_root = Path.home() / ".cache" / "mine-teleop" / "ubuntu-bundle-workspaces"
    shared_root.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="mine-teleop-ubuntu-bundle-", dir=str(shared_root)))


def _prepare_workspace(
    *,
    workspace: Path,
    repo_root: Path,
    chassis_control_root: Path,
    minepilot_root: Path,
    chassis_control_library: Path,
    bridge_library: Path | None,
) -> None:
    if workspace.exists():
        shutil.rmtree(workspace)
    workspace.mkdir(parents=True)
    _copy_tree(repo_root, workspace / "mine-teleop", excludes=(".git", "build", "dist", "__pycache__"))
    _copy_tree(chassis_control_root, workspace / "ChassisControl", excludes=("build", "__pycache__"))
    _copy_tree(minepilot_root, workspace / "MinePilot", excludes=("build", "__pycache__"))
    input_libs = workspace / "input-libs"
    input_libs.mkdir()
    if _container_path_for(chassis_control_library, chassis_control_root, minepilot_root).startswith("/workspace/input-libs/"):
        shutil.copy2(chassis_control_library, input_libs / "libchassis_control.so")
    if bridge_library is not None and _container_path_for(bridge_library, chassis_control_root, minepilot_root).startswith(
        "/workspace/input-libs/"
    ):
        shutil.copy2(bridge_library, input_libs / "libmine_teleop_chassis_bridge.so")


def _copy_tree(source: Path, destination: Path, *, excludes: tuple[str, ...]) -> None:
    if not source.is_dir():
        raise FileNotFoundError(source)
    if shutil.which("rsync"):
        command = ["rsync", "-a", "--delete"]
        for exclude in excludes:
            command.extend(["--exclude", exclude])
        command.extend([f"{source}/", f"{destination}/"])
        subprocess.run(command, check=True)
        return
    ignore = shutil.ignore_patterns(*excludes)
    shutil.copytree(source, destination, ignore=ignore)


def _container_path_for(path: Path | None, chassis_control_root: Path, minepilot_root: Path) -> str:
    if path is None:
        return ""
    for host_root, container_root in (
        (chassis_control_root, "/workspace/ChassisControl"),
        (minepilot_root, "/workspace/MinePilot"),
    ):
        try:
            relative = path.relative_to(host_root)
        except ValueError:
            continue
        return f"{container_root}/{relative}"
    if path.name == "libmine_teleop_chassis_bridge.so":
        return "/workspace/input-libs/libmine_teleop_chassis_bridge.so"
    return "/workspace/input-libs/libchassis_control.so"


def _docker_command_preview(*, workspace: Path, image: str, platform: str, container_script: str) -> str:
    quoted_script = shlex.quote(container_script)
    return "\n".join(
        (
            f"container_id=$(docker create --platform {shlex.quote(platform)} {shlex.quote(image)} bash -lc {quoted_script})",
            f"docker cp {shlex.quote(str(workspace))}/. \"$container_id:/workspace\"",
            'docker start -a "$container_id"',
            f"docker cp \"$container_id:/workspace/output\" {shlex.quote(str(workspace / 'docker-cp'))}",
            'docker rm "$container_id"',
        )
    )


def _run_docker_build(*, workspace: Path, image: str, platform: str, container_script: str) -> Path:
    create = subprocess.run(
        ["docker", "create", "--platform", platform, image, "bash", "-lc", container_script],
        text=True,
        capture_output=True,
        check=True,
    )
    container_id = create.stdout.strip()
    if not container_id:
        raise RuntimeError("docker create did not return a container id")
    output_parent = workspace / "docker-cp"
    shutil.rmtree(output_parent, ignore_errors=True)
    output_parent.mkdir()
    try:
        subprocess.run(["docker", "cp", f"{workspace}/.", f"{container_id}:/workspace"], check=True)
        subprocess.run(["docker", "start", "-a", container_id], check=True)
        subprocess.run(["docker", "cp", f"{container_id}:/workspace/output", str(output_parent)], check=True)
    finally:
        subprocess.run(["docker", "rm", "-f", container_id], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return output_parent / "output"


def _container_build_script(
    *,
    build_bridge: bool,
    container_chassis_control_library: str,
    container_bridge_library: str,
) -> str:
    manifest = {
        "event": "mine_teleop_ubuntu_bundle",
        "target_platform": "linux/amd64",
        "executable": "bin/mine-teleop",
        "dynamic_libraries": [
            "lib/libmine_teleop_chassis_bridge.so",
            "lib/libchassis_control.so",
        ],
        "media_tools": [
            "bin/ffmpeg",
            "bin/ffprobe",
            "bin/vainfo",
            "lib/dri/iHD_drv_video.so",
        ],
        "manual_smoke": "manual-smoke.sh",
        "docs": [
            "docs/15-ubuntu-bundle-software.md",
            "docs/16-ubuntu-bundle-usage.md",
            "docs/17-ubuntu-bundle-architecture.md",
        ],
        "smoke_commands": [
            "./manual-smoke.sh",
            "bin/mine-teleop --list",
            "bin/ffmpeg -hide_banner -hwaccels",
            "bin/mine-teleop vehicle-agent --config /etc/mine-teleop/vehicle-agent.yaml --adapter-status",
            "bin/mine-teleop target-host-validation-plan --mine-teleop-binary /opt/mine-teleop/bin/mine-teleop --format shell",
        ],
    }
    manifest_json = json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2)
    bridge_step = ""
    if build_bridge:
        bridge_step = textwrap.dedent(
            f"""
            python3 scripts/chassis_bridge_check.py \\
              --chassis-control-root /workspace/ChassisControl \\
              --minepilot-root /workspace/MinePilot \\
              --chassis-control-branch UI_Test \\
              --minepilot-branch merge_ui_test \\
              --chassis-control-library {shlex.quote(container_chassis_control_library)} \\
              --build-dir build/ubuntu-chassis-bridge \\
              --build
            cp build/ubuntu-chassis-bridge/libmine_teleop_chassis_bridge.so /workspace/output/lib/
            """
        ).strip()
    elif container_bridge_library:
        bridge_step = f"cp {shlex.quote(container_bridge_library)} /workspace/output/lib/libmine_teleop_chassis_bridge.so"
    else:
        bridge_step = textwrap.dedent(
            """
            if ! command -v cc >/dev/null 2>&1; then
              echo "C compiler is required to build the no-CAN chassis bridge stub" >&2
              exit 2
            fi
            cat > build/ubuntu-chassis-bridge-stub.c <<'C'
#include "mine_teleop_chassis_bridge.h"

#include <string.h>

int mine_teleop_chassis_open(const char* can_interface)
{
    (void)can_interface;
    return -95;
}

int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count)
{
    (void)target_gear;
    (void)target_vx;
    (void)target_ax;
    (void)steering_values;
    (void)steering_count;
    return -95;
}

int mine_teleop_chassis_emergency_stop()
{
    return -95;
}

int mine_teleop_chassis_update_feedback(const struct MineTeleopChassisFeedback* feedback)
{
    (void)feedback;
    return -95;
}

int mine_teleop_chassis_poll_feedback(struct MineTeleopChassisFeedback* feedback)
{
    if (feedback != 0) {
        memset(feedback, 0, sizeof(*feedback));
    }
    return 1;
}

int mine_teleop_chassis_read_telemetry(struct MineTeleopChassisTelemetry* telemetry)
{
    if (telemetry != 0) {
        memset(telemetry, 0, sizeof(*telemetry));
    }
    return -95;
}

int mine_teleop_chassis_close()
{
    return 0;
}
C
            cc -shared -fPIC \\
              -Ideployments/chassis-control-bridge \\
              build/ubuntu-chassis-bridge-stub.c \\
              -Wl,-soname,libmine_teleop_chassis_bridge.so \\
              -o /workspace/output/lib/libmine_teleop_chassis_bridge.so
            """
        ).strip()

    script = textwrap.dedent(
        f"""
        set -euo pipefail
        cd /workspace/mine-teleop
        apt_get() {{
          apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 "$@"
        }}
        if ! python3 -m pip --version >/dev/null 2>&1; then
          apt_get update
          DEBIAN_FRONTEND=noninteractive apt_get install -y --no-install-recommends python3-pip python3-venv binutils file git
        fi
        apt_get update
        DEBIAN_FRONTEND=noninteractive apt_get install -y --no-install-recommends \\
          ffmpeg \\
          intel-media-va-driver \\
          vainfo
        python3 -m pip install --no-cache-dir PyYAML pyinstaller
        rm -rf build/ubuntu-executable dist/mine-teleop
        mkdir -p /workspace/output/bin /workspace/output/lib /workspace/output/lib/dri /workspace/output/configs /workspace/output/docs /workspace/output/manifest /workspace/output/scripts
        pyinstaller --clean --onefile --name mine-teleop \\
          --distpath build/ubuntu-executable/dist \\
          --workpath build/ubuntu-executable/work \\
          --specpath build/ubuntu-executable/spec \\
          --add-data /workspace/mine-teleop/scripts:scripts \\
          --add-data /workspace/mine-teleop/vehicle-agent:vehicle-agent \\
          --add-data /workspace/mine-teleop/vehicle-media-agent:vehicle-media-agent \\
          --add-data /workspace/mine-teleop/vehicle-uploader:vehicle-uploader \\
          --add-data /workspace/mine-teleop/driver-console:driver-console \\
          --add-data /workspace/mine-teleop/signaling-server:signaling-server \\
          /workspace/mine-teleop/mine_teleop/cli.py
        cp build/ubuntu-executable/dist/mine-teleop /workspace/output/bin/mine-teleop.real
        chmod +x /workspace/output/bin/mine-teleop.real
        cat > /workspace/output/bin/mine-teleop <<'SH'
#!/usr/bin/env bash
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")/.." && pwd)"
export LD_LIBRARY_PATH="$root/lib:${{LD_LIBRARY_PATH:-}}"
export LIBVA_DRIVERS_PATH="${{LIBVA_DRIVERS_PATH:-$root/lib/dri}}"
exec "$root/bin/mine-teleop.real" "$@"
SH
        chmod +x /workspace/output/bin/mine-teleop
        cp {shlex.quote(container_chassis_control_library)} /workspace/output/lib/libchassis_control.so
        {bridge_step}
        copy_shared_libs() {{
          for object in "$@"; do
            ldd "$object" 2>/dev/null | awk '/=>/ {{print $3}} /^[[:space:]]*\\// {{print $1}}' | while read -r lib; do
              [ -n "$lib" ] || continue
              [ -f "$lib" ] || continue
              case "$(basename "$lib")" in
                libc.so.*|ld-linux-*|linux-vdso.so.*)
                  continue
                  ;;
              esac
              cp -L -n "$lib" /workspace/output/lib/ || true
            done
          done
        }}
        install_media_tool() {{
          tool="$1"
          real_name="$2"
          real_path="$(command -v "$tool")"
          cp -L "$real_path" "/workspace/output/bin/$real_name"
          chmod +x "/workspace/output/bin/$real_name"
          copy_shared_libs "$real_path"
          cat > "/workspace/output/bin/$tool" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")/.." && pwd)"
export LD_LIBRARY_PATH="$root/lib:${{LD_LIBRARY_PATH:-}}"
export LIBVA_DRIVERS_PATH="${{LIBVA_DRIVERS_PATH:-$root/lib/dri}}"
exec "$root/bin/TOOL_REAL_NAME" "$@"
SH
          sed -i "s/TOOL_REAL_NAME/$real_name/g" "/workspace/output/bin/$tool"
          chmod +x "/workspace/output/bin/$tool"
        }}
        install_media_tool ffmpeg ffmpeg.real
        install_media_tool ffprobe ffprobe.real
        install_media_tool vainfo vainfo.real
        for driver in /usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so /usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so; do
          if [ -f "$driver" ]; then
            cp -L "$driver" "/workspace/output/lib/dri/$(basename "$driver")"
            copy_shared_libs "$driver"
          fi
        done
        cp configs/vehicle-agent.dev.yaml /workspace/output/configs/
        python3 - <<'PY'
from pathlib import Path
import yaml

path = Path("/workspace/output/configs/vehicle-agent.dev.yaml")
config = yaml.safe_load(path.read_text(encoding="utf-8"))
encoding = config.setdefault("hardware", {{}}).setdefault("encoding", {{}})
encoding["ffmpeg_binary"] = "/opt/mine-teleop/bin/ffmpeg"
encoding["ffprobe_binary"] = "/opt/mine-teleop/bin/ffprobe"
encoding["vainfo_binary"] = "/opt/mine-teleop/bin/vainfo"
encoding["libva_drivers_path"] = "/opt/mine-teleop/lib/dri"
path.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
PY
        cp configs/driver-console.dev.yaml /workspace/output/configs/
        cp scripts/run_vehicle_live_media.sh /workspace/output/scripts/
        chmod +x /workspace/output/scripts/run_vehicle_live_media.sh
        cp scripts/ipc_manual_smoke.sh /workspace/output/manual-smoke.sh
        chmod +x /workspace/output/manual-smoke.sh
        cp README.md /workspace/output/docs/README.md
        cp docs/14-current-status-and-ipc-deployment.md /workspace/output/docs/
        cp docs/15-ubuntu-bundle-software.md /workspace/output/docs/
        cp docs/16-ubuntu-bundle-usage.md /workspace/output/docs/
        cp docs/17-ubuntu-bundle-architecture.md /workspace/output/docs/
        file /workspace/output/bin/mine-teleop.real > /workspace/output/manifest/file.txt
        ldd /workspace/output/bin/mine-teleop.real > /workspace/output/manifest/ldd.txt || true
        """
    ).strip()
    script += "\ncat > /workspace/output/manifest/bundle_manifest.json <<'MINE_TELEOP_MANIFEST'\n"
    script += manifest_json
    script += "\nMINE_TELEOP_MANIFEST\n"
    return script


if __name__ == "__main__":
    raise SystemExit(main())
