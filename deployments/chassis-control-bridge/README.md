# JYR010 VCU Parallel-Control Bridge

This directory provides the stable C ABI loaded by the native C++
`DynamicLibraryVehicleAdapter`. The exported contract is declared in
`mine_teleop_chassis_bridge.h` and covers explicit parallel handshake,
control application, emergency stop, feedback polling, telemetry, and safe
disconnect.

The canonical packaging input is produced in Ubuntu 22.04 amd64 Docker:

```bash
MINE_TELEOP_CHASSIS_CONTROL_ROOT=/path/to/ChassisControl \
MINE_TELEOP_CHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so \
  scripts/build/prepare_chassis_runtime.sh
```

The script writes the two ignored runtime libraries to `vendor/chassis/lib`.
Every vehicle package requires both files; missing or unresolved libraries stop
the build instead of producing an incomplete artifact.

Build it on Ubuntu 22.04 after producing ChassisControl:

```bash
cmake -S deployments/chassis-control-bridge -B build/chassis-control-bridge \
  -DCHASSIS_CONTROL_ROOT=/path/to/ChassisControl \
  -DCHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so
cmake --build build/chassis-control-bridge --parallel
```

Copy `libmine_teleop_chassis_bridge.so`, `libchassis_control.so`, and their
non-glibc dependencies into the release bundle. Set
`vehicle_adapter.integration.chassis_control.bridge_library_path` to the
installed path, normally
`/opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so`.

The repository field template intentionally keeps `vehicle_adapter.type:
mock`. On an isolated CAN bench, edit the deployed
`/opt/mine-teleop/config/vehicle-agent.yaml`: choose `type: can`, set the
bridge path above and the same interface declared by `hardware.can.interface`
(`can1` in the current field template), give `field_safety.max_speed_kph` a
commissioning value of at least `1 km/h` (the VCU field resolution), and set the authoritative
`max_throttle`/`full_scale_motor_torque_nm`/`max_brake`/
`max_steering_angle_deg` limits. `full_scale_motor_torque_nm` is the steady
per-channel target at straight-line effective throttle 1.0 and brake 0; it is
multiplied by the vehicle `max_throttle`, and the ChassisControl 300 Nm/s slew
remains active. After ChassisControl steering compensation and rate limiting,
the bridge applies a final symmetric per-motor clamp of
`full_scale_motor_torque_nm * normalized_throttle`, including reverse torque.
While D or R traction is requested without braking, `max_speed_kph` is the
speed request and throttle controls the torque request.
Releasing traction, applying brake, or configuring zero traction
torque withdraws the speed request as `0 km/h / Q=0`; D/R gear and steering
authority remain unchanged. A valid positive speed request is never emitted
without positive traction capability.
`max_brake` limits ordinary driving commands only; safety-stop braking bypasses
it. The browser limit dialog can only reduce those vehicle-side limits.
Any positive brake request dominates throttle. If ChassisControl rejects an
update or returns a non-finite actuator value, the bridge latches a local
emergency stop immediately instead of retaining the previous traction command
until the upstream watchdog expires.

The runtime passes `control.control_timeout_ms` through the versioned bridge
open ABI. While the VCU controller is Ready, the bridge withdraws the speed
request, commands zero torque and calibrated safety braking if no successful
upstream apply arrives before that deadline. The event is logged once as
`vcu_control_apply_timeout`; the next valid apply clears this watchdog latch.

Upgrade the runtime and this bridge together. The current runtime requires
`mine_teleop_chassis_open_v2`; a bridge that only exports V1 fails before any
CAN initialization instead of silently ignoring the watchdog configuration.
The bridge continues to export `mine_teleop_chassis_open_v1` for older runtimes,
using the default 800 ms control timeout on that compatibility path.

Validate before service startup:

```bash
/opt/mine-teleop/bin/mine-teleop-run config-check \
  --config /opt/mine-teleop/config/vehicle-agent.yaml

/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --preflight

/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --adapter-status
```

The bridge uses SocketCAN on Linux and the repository-owned JYR010 20260714
codec. ChassisControl supplies the eight-wheel dynamics/control calculation.
It does not use MinePilot's older generated CAN codec at runtime.

Bridge open starts in standby. It sends the 16 low-request ADU frames every
20 ms but does not request driving authority. An active driver must click the
controller's start button, and the bridge accepts that request only when fresh
feedback confirms:

- physical selector N (`WVCU_GearCtrlReqSts=1`);
- absolute speed no greater than 0.1 m/s;
- all four EPBs parked (value 2);
- VCU manual handshake state 3.

The parallel-driving feature intentionally reuses the vehicle's proven
intelligent-driving handshake path:
`ShakeReq=2 -> WVCU_ShakeHandSts=5`, while `CloudShakeReq` remains zero.
The controller receives the detailed handshake status over the same
DataChannel and enables driving commands only at `ready`.

Disconnect performs the full reverse sequence: zero torque, zero speed, N,
EPB park, clear `ShakeReq`, and wait for manual state 3.

The required JSONL protocol log defaults to
`/var/log/mine-teleop/vcu-can.jsonl` and includes every raw recognized RX
frame, every 20 ms TX batch, physical control parameters, state transitions,
accepted/rejected handshake commands with all gate values, faults, and
disarm results. Configure rotation with `MINE_TELEOP_VCU_LOG_MAX_BYTES` and
`MINE_TELEOP_VCU_LOG_ROTATIONS`.

A configured dynamic adapter fails startup if the bridge, CAN interface, or
log path is unavailable; it never falls back to the mock adapter. Real
CAN/VCU acceptance remains a separate bench/vehicle task.
