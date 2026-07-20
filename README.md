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
- `cpp/src/media.cpp`: direct V4L2 mmap capture, vendor-SDK camera capture,
  native JPEG test frames, and MP4 recording.
- `cpp/src/upload.cpp`: atomic sidecar scanning, SHA-256 verification,
  bandwidth limiting, and resumable local archive upload.
- `cpp/bridges/mvs_camera_bridge.cpp`: optional Hikrobot MVS bridge.
- `scripts/pylon_camera_bridge.cpp`: optional Basler pylon bridge.

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

The resulting `dist/cpp-ubuntu22.04-*` directory contains the executable,
FFmpeg/ffprobe for H.264 recording and MP4 muxing, V4L2/VAAPI tools, CA
certificates, and all non-glibc shared
libraries. The only host ABI baseline is Ubuntu 22.04 glibc/kernel. The launcher
sets bundle-relative `PATH`, `LD_LIBRARY_PATH`, and `SSL_CERT_FILE`.

For the chassis integration or licensed Hikrobot/Basler cameras, place
redistributable files under `vendor/chassis`, `vendor/mvs`, or `vendor/pylon`
before building. See `vendor/README.md`. Their bridge and shared libraries are
then carried in the same bundle; the target x64 Ubuntu host does not need an SDK
installation or a source checkout.

## Commands

```bash
./mine-teleop version
./mine-teleop config-check --config /etc/mine-teleop/vehicle-agent.yaml

./mine-teleop signaling-server --host 0.0.0.0 --port 8765
./mine-teleop driver-console --host 0.0.0.0 --port 8080

./mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --preflight

./mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --teleop --service

./mine-teleop vehicle-media-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --driver-console-url http://control-host:8080 \
  --service

./mine-teleop vehicle-uploader \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --recording-root /var/lib/mine-teleop/recordings \
  --archive-root /var/lib/mine-teleop/uploader/archive \
  --service
```

Credentials can be supplied with `MINE_TELEOP_DRIVER_PASSWORD` and
`MINE_TELEOP_DEVICE_TOKEN`. For systemd, install the units from
`deployments/systemd` and create `/etc/mine-teleop/mine-teleop.env` from the
provided example.

## Development control plane

```bash
scripts/run_control_plane_docker_smoke.sh
scripts/run_control_plane_docker.sh
```

The smoke creates native signaling and console containers, grants one vehicle
session, sends a control command, pulls it as the vehicle, generates a real JPEG
in C++, posts it to the console, and checks the retained frame status.

## Operational boundaries

- V4L2 acquisition uses native C++ `ioctl`/mmap/poll and accepts camera-native
  MJPEG; it does not launch FFmpeg. The test source is generated and JPEG
  encoded in-process.
- FFmpeg is reserved for H.264 recording and MP4 muxing. It is carried in the
  bundle and is not expected to exist on the target host.
- MVS and pylon are compiled only when their redistributable SDK bundle is
  supplied; selecting one without its bridge fails explicitly.
- `mock` is for bench validation only. Field configurations should use the
  dynamic chassis bridge and require CAN feedback.
- The bundle does not carry the Ubuntu kernel, device drivers, or glibc. Those
  are the declared Ubuntu 22.04 host baseline; every application-level library
  is carried by the bundle.
