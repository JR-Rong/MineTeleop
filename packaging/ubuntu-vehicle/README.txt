Mine Teleop Vehicle for Ubuntu 22.04 x64
=========================================

This package is a self-contained native vehicle runtime. It does not require
Python, FFmpeg, a source checkout, or a systemd service. Linux kernel drivers,
camera/GPU device nodes, and their access permissions remain host
prerequisites.

First start:

  cd /path/to/this/package
  printf '%s\n' '<device-token-from-secret-store>' > config/device-token
  chmod 600 config/device-token
  sudo install -d -m 0750 -o "$(id -un)" -g "$(id -gn)" /var/log/mine-teleop
  ./bin/mine-teleop-run

The foreground launcher starts the configured control and media services.
Stop it with Ctrl-C. It preserves stdout/stderr in the terminal and also writes
them to /var/log/mine-teleop/vehicle-runtime.log. That mixed runtime log rotates
at 64 MiB with five retained files; high-frequency CAN evidence remains in the
separately rotated vcu-can.jsonl. The vendor ChassisControl per-cycle state
dump (debug/info lines in its "[timestamp] [level] [pid] [tag]" format) stays
on the terminal only and is not persisted; vendor warning/error lines and all
structured runtime diagnostics are still recorded. The default field
configuration uses the mock adapter and
max_speed_kph=0; do not enable a physical chassis before the separate CAN,
braking, and local emergency-stop acceptance is complete.

Camera count:

  Each cameras entry with enabled: true in config/vehicle-agent.yaml creates
  one WebRTC video track. The packaged field configuration enables the stable
  UVC path front_uvc and Basler serial 25192546. Changing the enabled set
  requires a media-session restart.

ICE policy:

  cloud.ice_transport_policy: all gathers direct, STUN, and TURN candidates and
  selects the best available route. Set it to relay only for a forced-TURN
  acceptance test; relay consumes cloud bandwidth and normally adds latency.

Basler USB access:

  sudo ./scripts/setup_basler_usb_access.sh "$USER"

Log out and back in after the group change, then verify discovery:

  LD_LIBRARY_PATH="$PWD/lib" ./bin/mine-teleop-aravis-camera --list --json

Diagnostics:

  tail -F /var/log/mine-teleop/vehicle-runtime.log
  ./bin/mine-teleop-run version
  ./bin/mine-teleop-run config-check --config config/vehicle-agent.yaml \
    --chassis-bridge-library lib/vendor/chassis/libmine_teleop_chassis_bridge.so
  ./bin/mine-teleop-run vehicle-agent --config config/vehicle-agent.yaml --preflight

The runtime log can be overridden with MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH,
MINE_TELEOP_VEHICLE_RUNTIME_LOG_MAX_BYTES, and
MINE_TELEOP_VEHICLE_RUNTIME_LOG_ROTATIONS. Only the long-running
vehicle-runtime command uses this log; one-shot diagnostics stay on the terminal.

The package intentionally contains no device token. Vehicle registration uses
a TLS-protected POST body; follow-up GET/WSS authentication uses
X-Mine-Teleop-Device-Token headers, not URL query strings. The server retains
query-string fallback only for older clients during migration.
