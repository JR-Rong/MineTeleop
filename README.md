# Mine Teleop

Mine Teleop is now a native C++20 runtime for Ubuntu 22.04. Production control,
signaling, driver-console, vehicle-media, recorder/uploader, and camera bridge
entry points do not require Python.

## Runtime layout

- `cpp/src/core.cpp`: control command validation, last-value mailbox, safety
  state machine, vehicle adapter, telemetry, and YAML configuration.
- `cpp/src/http.cpp`: bundled-cURL client and vehicle signaling loop.
- `cpp/src/server.cpp`: native HTTP server, authenticated signaling service,
  and browser driver console.
- `cpp/src/media.cpp`: direct V4L2 mmap capture, vendor-SDK camera capture, and
  native JPEG test frames.
- `cpp/src/video.cpp`: unified hardware `VideoEncoder` selection with NVIDIA
  NVENC first and Intel VAAPI second.
- `cpp/src/webrtc_media.cpp`: multi-camera WebRTC/SRTP publishing and
  encoded-packet MP4 segmentation.
- `cpp/src/upload.cpp`: atomic sidecar scanning, SHA-256 verification,
  bandwidth limiting, and resumable local archive upload.
- `cpp/bridges/mvs_camera_bridge.cpp`: optional Hikrobot MVS bridge.
- `cpp/bridges/aravis_camera_bridge.cpp`: minimal Aravis/libusb USB3 Vision
  bridge for Basler and other GenICam cameras.

The stable chassis adapter ABI is in
`deployments/chassis-control-bridge/mine_teleop_chassis_bridge.h`. A production
vehicle integration is loaded with `dlopen`; a missing vendor adapter cannot
silently fall back to mock control. Its compiled bridge, ChassisControl library,
and transitive redistributable libraries belong under `vendor/chassis/lib` and
are copied into the bundle; no checkout or library path from the target PC is
discovered at runtime.

## Build and test

The canonical build environment is Ubuntu 22.04:

```bash
docker build --target build \
  -f deployments/cpp/Dockerfile.build .

scripts/check.sh
```

The build runs the native CTest suite, configuration validation, a deterministic
safety timeout loop, authenticated driver-to-vehicle signaling, a native C++
test-source frame path, and atomic uploader checks.

## Self-contained Ubuntu x64 bundle

Build the Ubuntu 22.04 x86_64/amd64 bundle from any Docker-capable host:

```bash
scripts/build_cpp_ubuntu_bundle.sh linux/amd64
```

The exported artifact directory contains x86-64 ELF executables and shared
libraries under `bin/` and `lib/`, plus the ready-to-edit vehicle configuration
under `config/vehicle-agent.yaml`. It carries GStreamer WebRTC/RTP/MP4 plugins,
Intel's VAAPI userspace driver, and the Ubuntu 22.04 dynamic loader/glibc. It
does not carry FFmpeg, Python, credentials, or shell launchers. The static
`bin/mine-teleop-run` launcher discovers the bundle location, configures the
bundled loader/media paths, and starts the configured C++ vehicle services.
The device token remains outside the package. NVIDIA's kernel
driver remains a hardware/OS prerequisite; the matching redistributable NVIDIA
userspace libraries must be copied into `lib/` for a field package.

For the chassis integration or licensed Hikrobot cameras, place redistributable
files under `vendor/chassis` or `vendor/mvs` before building. See
`vendor/README.md`. Basler USB3 Vision cameras use the pinned, library-only
Aravis/libusb build and do not require pylon. All bridge shared libraries are
carried in the bundle; the target x64 Ubuntu host does not need an SDK
installation or source checkout.

## Commands

```bash
package_root=/opt/mine-teleop
printf '%s\n' 'replace-with-device-token' > "$package_root/config/device-token"
chmod 600 "$package_root/config/device-token"

# One foreground command: control + media + recording from the bundled YAML.
"$package_root/bin/mine-teleop-run"
```

For diagnostics or individual subcommands, pass the native command to the same
launcher:

```bash
mt="$package_root/bin/mine-teleop-run"
"$mt" version
"$mt" config-check --config "$package_root/config/vehicle-agent.yaml"
"$mt" time-sync --signaling-http-url http://control-host:8765 --samples 7 --max-uncertainty-ms 25

"$mt" vehicle-agent \
  --config "$package_root/config/vehicle-agent.yaml" \
  --preflight
"$mt" media-probe
```

Credentials can still be supplied with `MINE_TELEOP_DRIVER_PASSWORD` and
`MINE_TELEOP_DEVICE_TOKEN`; the default vehicle config instead reads
`config/device-token`. Vehicle deployment is foreground-only: the native
`vehicle-runtime` supervisor starts both configured services and terminates the
peer if either process fails. The deployment flow does not install or require
systemd units. Without an external USB ACL,
the media command must run as root so libusb can open a Basler USB3 Vision
device for control and streaming.

Both vehicle processes and the driver console synchronize to the signaling
server's application time domain before opening a session. They use 7
four-timestamp samples, select the lowest-RTT samples, report offset/RTT/
uncertainty, and refresh every 30 seconds. With `require_time_sync: true`, the
vehicle refuses remote operation when uncertainty exceeds
`max_time_sync_uncertainty_ms`; no host NTP package is required for relative
latency measurements. The browser sends the current control state at 20 Hz;
non-emergency commands older than `max_command_gap_ms` (or too far in the
future) are rejected in the shared time domain, while emergency stop remains
available regardless of command age.

## Development control plane

```bash
scripts/run_control_plane_docker_smoke.sh
scripts/run_control_plane_docker.sh
```

The smoke creates native signaling and console containers, grants one vehicle
session, verifies control-message isolation, and exchanges browser media codec
capabilities without using the removed per-frame upload path.

## Operational boundaries

- V4L2 acquisition uses native C++ `ioctl`/mmap/poll and accepts camera-native
  MJPEG; it does not launch FFmpeg. The test source is generated and JPEG
  encoded in-process.
- Browser playback uses native WebRTC continuous video. H.265 is selected only
  when the browser advertises it. If browser stats report any H.265 track below
  20 fps for three consecutive samples, the vehicle skips the remaining H.265
  backend and renegotiates H.264, preferring NVENC before VAAPI.
- Realtime encoding defaults to NVENC and falls back to Intel VAAPI. Recording
  tees the already encoded H.264/H.265 access units into `splitmuxsink/mp4mux`;
  no second encoder process is launched.
- The Aravis/libusb bridge is built from a pinned source revision with USB3
  Vision enabled and viewer, GStreamer plugin, introspection, documentation,
  tests, and packet-socket support disabled. `aravis:`, `basler:`, and legacy
  `pylon:` device selectors all use this bridge; no pylon library is loaded.
- MVS is compiled only when its redistributable SDK bundle is supplied;
  selecting it without its bridge fails explicitly.
- `mock` is for bench validation only. Field configurations should use the
  dynamic chassis bridge and require CAN feedback.
- The strict test artifact carries its userspace dynamic loader and libraries,
  but not the Ubuntu kernel or kernel-mode GPU/camera drivers.
