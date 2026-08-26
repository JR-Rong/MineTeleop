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

The repository field template intentionally keeps `vehicle_adapter.type: mock`.
On an isolated CAN bench, edit the deployed
`/opt/mine-teleop/config/vehicle-agent.yaml`: choose `type: can`, set the
bridge path above and the same interface declared by `hardware.can.interface`
(`can1` in the current field template), set a commissioning
`field_safety.max_speed_kph`, and explicitly configure the local speed PID,
speed-feedback deadline, hard-overspeed margin, `max_throttle`,
`full_scale_motor_torque_nm`, `max_brake_pressure_bar`, and steering limits.

Before ordinary driving, the controller submits a complete `profile_version=2`
snapshot containing target speed, maximum per-motor torque, maximum ordinary
EHB pressure, service and hard-brake pressure, maximum steering, and the five
speed-PID settings. The vehicle applies and acknowledges
that profile before it permits the VCU handshake. The profile can only reduce
the immutable vehicle-side YAML limits and is cleared on disconnect, authority
or session replacement, adapter-owned safety stop, and fault. First apply,
steering/PID/brake changes, and target/torque increases require fresh parked-N
feedback in Standby or Disarmed. A clear withdraws traction, resets the PID,
and restores the YAML PID defaults without clearing a safety latch. Feedback
timing, overspeed, command timeouts, and other reported safety settings remain
vehicle-side read-only. A successful `applied_revision` equals the request seq.

The runtime converts analog throttle to a local target speed within the
acknowledged session target. The bridge's single
SocketCAN I/O thread runs the PID once per 20 ms cycle from fresh signed VCU
speed feedback. Any positive throttle enables the PID, but is not a second
torque ceiling. The PID output is clamped to `[0, 1]`; at the target the
integral term may retain positive torque. ChassisControl maps that normalized
output to eight channels, after which the bridge clamps D to positive-only and
R to negative-only torque, quantizes toward zero at 0.1 Nm, and limits every
channel to the smaller of the acknowledged session limit and
`full_scale_motor_torque_nm`. A fixed 0.05 m/s setpoint-reference
deadband preserves the integral through small gamepad jitter; cumulative target
movement beyond that band, including a material target decrease, resets it.

`ADU_Tx_VehSpdReq` is intentionally always encoded as `0 km/h / Q=0`; the
runtime does not depend on the unverified VCU target-speed loop. Any brake
zeros throttle and target speed, resets the PID, and forces all eight motor
torques to zero while retaining steering. ABI V3 transports the acknowledged
physical brake pressure through the apply call; the bridge invokes
ChassisControl for steering, then overwrites all eight EHB channels with the
direct pressure quantized to 0.1 bar. No
traction, stale/invalid speed, non-Ready state, a gear mismatch, or an abnormal
PID interval likewise resets the PID and commands zero traction.

The DBC defines every MCU torque request at 0.1 Nm resolution over
`[-800, 838.3] Nm`. The ordinary-driving symmetric code ceiling is
`min(800 * 0.8, 838.3 * 0.8)`, quantized toward zero to `640.0 Nm` per motor.
The vehicle and controller defaults are `300 Nm` per motor. The symmetric cap
is exactly 80% of the reverse bound and about 76.34% of the forward bound.
Recalculate migrated limits and repeat isolated bench calibration before using
a real adapter; these are CAN request limits, not measured motor or wheel
torque. The field template's PID gains are schema-bounded placeholders, not
vehicle calibration.

The DBC defines each EHB01/EHB02 pressure request as a 12-bit value at 0.1 bar
resolution over `0..409.5 bar`. The ordinary-driving code ceiling is 80%, or
`327.6 bar` per channel. The vehicle-side default maximum is 100 bar; the
controller defaults to maximum/service/hard pressures of `100/30/100 bar`.
Service brake is a direct pressure step in this change, not a separate brake
PID or pressure/jerk ramp; hard brake directly requests its configured session
pressure. ESTOP, physical emergency, bridge/ChassisControl fault, disconnect,
and the bridge-local apply watchdog bypass ordinary limits and request the full
DBC `409.5 bar` on all eight channels. The upstream control timeout first sends
its 0.3/0.6 stages through the ordinary pressure path and only its final 1.0
stage requests full 409.5 bar. If ChassisControl rejects an update or returns a
non-finite actuator value, the bridge latches a local emergency stop immediately
instead of retaining the previous traction command until the upstream watchdog
expires.

The runtime passes `control.control_timeout_ms` through the versioned bridge
open ABI. After a session first reaches Ready, this apply watchdog remains
armed through `WaitGear` and `WaitActuatorModes`. If no successful upstream
apply arrives before the deadline, the bridge commands zero torque and
full-DBC 409.5 bar safety braking and logs `vcu_control_apply_timeout` once. A timeout
in Ready can be cleared by the next valid Ready apply; a timeout during a shift
enters the reverse disarm sequence and must complete that sequence before a new
handshake.

The configurable speed-feedback deadline is independent from the 500 ms
all-feedback watchdog. `WaitParkingBrakeReleased`, `WaitGear`, and
`WaitActuatorModes` are stationary convergence phases: fresh absolute speed
above 0.1 m/s latches a safe stop and immediately enters the reverse sequence.
Each first-arming phase also has a 500 ms entry grace; after that deadline its
state-specific required feedback must remain fresh, so CAN silence cannot leave
EPB release asserted indefinitely. After the session reaches Ready, all 29
critical feedback IDs remain watched across later gear/mode waits. In Ready,
including zero-traction or braking intents, speed beyond
`max_speed_kph + hard_overspeed_margin_kph`, or signed motion opposite the
selected D/R direction by more than 0.1 m/s, latches a local safe stop. Ordinary
apply calls cannot clear it. Recovery requires the full disarm sequence, fresh
valid zero speed, N, all EPBs parked and manual VCU state, followed by an
explicit new parallel-handshake request.

Upgrade the vehicle-agent runtime and this bridge atomically. The current
runtime requires
ABI version 3, an exact V3 struct-size match, and
`mine_teleop_chassis_open_v3` plus the additive
`mine_teleop_chassis_apply_state_v2`; the runtime queries these capabilities before any CAN
initialization. A V1/V2-only bridge fails closed instead of silently ignoring the
physical ordinary-brake ceiling or interpreting the negative apply value as
legacy deceleration.
ABI version 3 also requires the runtime-control V1 size query plus
`mine_teleop_chassis_configure_runtime_control_v1` and
`mine_teleop_chassis_clear_runtime_control_v1`; missing symbols or a wrong POD
size are rejected before SocketCAN initialization.
`apply_state_v2` returns a fixed-size POD result in the same locked call. It
distinguishes the D/R moving-or-stale gate from other rejected applies without
exporting bridge log strings or requiring a racy last-error getter. The legacy
`apply_state` symbol remains a fail-closed integer wrapper.
The bridge continues to export `mine_teleop_chassis_open_v1` and
`mine_teleop_chassis_open_v2` for direct ABI callers and compatibility tests.
V1 deliberately disables traction because it cannot supply a validated
local-PID safety configuration; V2 retains its legacy negative-deceleration
apply semantics. These exports do not let an older vehicle-agent pass the
global ABI-version-3 startup gate.

Validate before service startup:

```bash
/opt/mine-teleop/bin/mine-teleop-run config-check \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --chassis-bridge-library /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so

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

This 20 ms period is the bridge's fixed 50 Hz SocketCAN I/O/PID loop. The
upstream `control.rate_hz` is independently fixed at 20 Hz (50 ms).

The parallel-driving feature intentionally reuses the vehicle's proven
intelligent-driving handshake path:
`ShakeReq=2 -> WVCU_ShakeHandSts=5`, while `CloudShakeReq` remains zero.
The controller receives the detailed handshake status over the same
DataChannel and enables driving commands only at `ready`.

Drive-gear changes require fresh valid gear and speed feedback at or below
0.1 m/s in both the bridge and the VCU state machine. A rejected change first
withdraws the previous traction command. While `WaitGear` or
`WaitActuatorModes` completes, steering and requested EHB pressure remain
continuous but all eight motor torque requests remain zero.

An ESTOP cannot let an arming state continue forward. In
`WaitParallelHandshake` it withdraws ShakeReq while retaining EPB park; from
`WaitParkingBrakeReleased` onward it immediately enters the EHB-braked reverse
sequence.

The WVCU physical emergency switch is latched when VehicleStatus is ingested,
so even an asserted-and-released pulse drained within one 20 ms cycle cannot
restore an old traction intent. Ordinary apply/soft-clear calls cannot reset
this latch. After the switch is released, recovery requires the complete
Disarmed sequence plus fresh actual N, zero speed, EPB park, manual state, and
an explicit new handshake request.

Disconnect performs the full reverse sequence: zero torque, N,
EPB park, clear `ShakeReq`, and wait for manual state 3.

The required JSONL protocol log defaults to
`/var/log/mine-teleop/vcu-can.jsonl` and includes every raw recognized RX
frame, every 20 ms TX batch, physical control parameters, state transitions,
accepted/rejected handshake commands with all gate values, faults, and
disarm results. Configure rotation with `MINE_TELEOP_VCU_LOG_MAX_BYTES` and
`MINE_TELEOP_VCU_LOG_ROTATIONS`.

A configured dynamic adapter fails startup if the bridge, CAN interface, or
log path is unavailable; it never falls back to the mock adapter. Real
CAN/VCU acceptance remains a separate bench/vehicle task. Successful builds,
unit tests, bundle checks, and virtual-CAN tests do not establish real vehicle
speed-control or braking acceptance.
