# ChassisControl Bridge

This directory contains the C shim used by `DynamicLibraryVehicleAdapter`.
It wraps the C++ ChassisControl API in a stable C ABI that Python can load with
`ctypes`. The ABI surface is declared in `mine_teleop_chassis_bridge.h`, so the
target build, Python structure definitions, and bench-review checklist all use
the same telemetry, feedback, control, estop, and close symbols.

Before building, run the bridge prerequisite check. On the macOS development
machine this validates repository paths, required external branches, MinePilot
CAN headers/sources, the selected `libchassis_control`, and CMake configure. On the
target vehicle host, use the same command before the actual build:

```bash
python3 scripts/chassis_bridge_check.py \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --build-dir build/chassis-control-bridge
```

The command emits JSONL: the first line is `chassis_bridge_check`, followed by
one record per prerequisite. It returns 0 only when every prerequisite is
`ready` or explicitly `skipped`; otherwise it returns 2.
By default the checker expects ChassisControl on `UI_Test` and MinePilot on
`merge_ui_test`; pass `--chassis-control-branch` or `--minepilot-branch` only
when intentionally validating a different integration branch.

Build on the target Ubuntu vehicle host after building ChassisControl:

```bash
cmake -S deployments/chassis-control-bridge -B build/chassis-control-bridge \
  -DCHASSIS_CONTROL_ROOT=/Volumes/SystemDisk/Workspace/ChassisControl \
  -DMINEPILOT_ROOT=/Volumes/SystemDisk/Workspace/MinePilot
cmake --build build/chassis-control-bridge
```

The bridge CMake search path accepts either a ChassisControl build output under
`CHASSIS_CONTROL_ROOT` or a MinePilot-provided `libchassis_control` under
`MINEPILOT_ROOT`, matching the current `merge_ui_test` integration layout. If
the library is elsewhere, pass
`-DCHASSIS_CONTROL_LIBRARY=/absolute/path/to/libchassis_control.so`.
When the selected library is under `MINEPILOT_ROOT`, CMake also uses
`MINEPILOT_ROOT/chassis_control.h` as `CHASSIS_CONTROL_API_ROOT` so the C++
`ArmingFeedback` layout matches the linked library. Override
`-DCHASSIS_CONTROL_API_ROOT=/path/to/api/header/root` only when the library was
built against a different header root.

This bridge must be built on a Linux SocketCAN or supported Windows CAN host.
The macOS development machine can configure the target and validate CMake
selection, but ChassisControl `can_common.h` intentionally rejects unsupported
platforms during compilation.

Then configure `vehicle_adapter.integration.chassis_control.abi: c_shim` and
set `bridge_library_path` to the built `libmine_teleop_chassis_bridge.so`.

Runtime CAN receive integration decodes WVCU feedback with the MinePilot
`can_db`/receiver code through `mine_teleop_chassis_poll_feedback`, while
ChassisControl `SendCanMessage` remains the bridge send path backed by the
linked chassis library and MinePilot sender implementation. CMake validates
`can_db.h`, `can_receiver.h`, and
`can_sender.h` plus `src/can_db.cpp`, `src/can_receiver.cpp`, and
`src/can_sender.cpp` are all present, and the bridge preflight reports the same
paths explicitly, so the target build cannot silently omit either direction of
the CAN integration. The Python `DynamicLibraryVehicleAdapter` calls
`poll_feedback()` before telemetry reads; each decoded snapshot is converted
into handshake, parking, gear, mode, steering-angle, and speed feedback and
forwarded into the ChassisControl arming feedback cache. Missing steering-angle
feedback is forwarded as NaN for ChassisControl builds that gate arming
readiness on wheel centering. Feedback polling uses the linked
`can_receive(..., 0)` API in non-blocking mode; a timeout means no fresh frame
is available and returns 1 to Python without delaying the telemetry/control
loop.
