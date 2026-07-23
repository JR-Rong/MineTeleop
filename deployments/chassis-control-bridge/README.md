# ChassisControl Bridge

This directory provides the stable C ABI loaded by the native C++
`DynamicLibraryVehicleAdapter`. The exported contract is declared in
`mine_teleop_chassis_bridge.h` and covers open, control application, emergency
stop, feedback polling, telemetry, and close.

Build it on Ubuntu 22.04 after producing ChassisControl and MinePilot:

```bash
cmake -S deployments/chassis-control-bridge -B build/chassis-control-bridge \
  -DCHASSIS_CONTROL_ROOT=/path/to/ChassisControl \
  -DMINEPILOT_ROOT=/path/to/MinePilot \
  -DCHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so
cmake --build build/chassis-control-bridge --parallel
```

Copy `libmine_teleop_chassis_bridge.so`, `libchassis_control.so`, and their
non-glibc dependencies into the release bundle. Set
`vehicle_adapter.integration.chassis_control.bridge_library_path` to the
bundle-relative installed path.

Validate before service startup:

```bash
/opt/mine-teleop/mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --preflight

/opt/mine-teleop/mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status
```

The bridge uses SocketCAN on Linux, ChassisControl for control output, and the
MinePilot CAN database/receiver for decoded vehicle feedback. A configured
dynamic adapter fails startup if the bridge or CAN interface is missing; it
never falls back to the mock adapter.
