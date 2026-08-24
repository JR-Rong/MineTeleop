# Mine Teleop

Mine Teleop is now a native C++20 runtime. Vehicle media targets Ubuntu 22.04;
the separated browser-based control client now builds natively on macOS arm64.
Production control, signaling, driver-console, vehicle-media, recorder/uploader,
and camera bridge entry points do not require Python.

## Guides

- [编译教程](docs/BUILD.md)：从源码生成云端、控制端和车端安装包；
- [部署教程](docs/DEPLOY.md)：凭据准备、三端安装、启动、验收和回滚；
- [测试与验收](docs/11-testing-and-validation.md)：自动化测试和硬件验收边界。

## Runtime layout

- `cpp/src/core.cpp`: control command validation, last-value mailbox, safety
  state machine, vehicle adapter, telemetry, and YAML configuration.
- `cpp/src/http.cpp`: bundled-cURL client and vehicle signaling loop.
- `cpp/src/websocket.cpp`: RFC 6455 handshake/frame transport for WSS clients
  and the authenticated server push channel.
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

scripts/test/check.sh
```

The build scripts compile and package only by default. Pass the exact `test`
argument to a build script to compile its test targets and run its test gates.
`scripts/test/check.sh` remains the explicit full validation entrypoint.

### macOS cloud bundle

Build the Ubuntu 22.04 x86_64 cloud package from an Intel or Apple Silicon Mac
with Docker Desktop:

```bash
scripts/build/build_macos_cloud_bundle.sh
```

Append `test` to compile and run the native tests and the extracted-package
self-test:

```bash
scripts/build/build_macos_cloud_bundle.sh test
```

This uses a dedicated signaling-only Dockerfile and does not build GStreamer,
camera bridges, or the vehicle runtime. It emits
`dist/mine-teleop-cloud-ubuntu22.04-x64-YYYYMMDD-HHMMSS.tar.gz` plus a SHA-256
file. The final package is self-contained for the native signaling binary and
also carries the systemd, Caddy, HAProxy, and coturn deployment assets.

Cloud installation, credentials, startup, health checks, and rollback are kept
separately in [docs/DEPLOY.md](docs/DEPLOY.md).

### macOS control client

Build, sign, and package the native control client without GStreamer or camera
dependencies:

```bash
scripts/build/build_macos_control_bundle.sh
```

To compile and run the test targets and validate the final archive:

```bash
scripts/build/build_macos_control_bundle.sh test
```

The generated `dist/mine-teleop-control-macos-arm64-*.tar.gz` contains the
executable, shared YAML, protocol-v1 vectors, CA bundle, build proof, and
`run-control.command`. It uses the macOS Security/CommonCrypto system APIs and
does not require Homebrew libraries. The process binds only to `127.0.0.1`, opens
the default browser, and emits a clear error if the configured port is busy.
With `test`, the script also runs `scripts/test/check_macos_control_bundle.sh`
against the final archive. It verifies checksum, signature, architecture,
dependencies, contents, extracted startup, loopback and port-conflict behavior,
page syntax, the redacted local event log, and clean shutdown. A default build
does not run these tests and records `runtime_tests_executed=no`.
The native client keeps tokens outside browser JavaScript, uses HTTPS for the
session API, and uses real WSS push/ack for offer/answer/ICE signaling. Server
push remains queued until an authenticated delivery-cursor ACK; reconnects
deduplicate replay, and uncertain client sends retry the same stable message ID.
Public plaintext endpoints are rejected; the packaged CA bundle is applied explicitly
to both HTTPS and WSS libcurl handles.
See [docs/21-macos-control-client.md](docs/21-macos-control-client.md).

### Server TLS/WSS entry

`mine-teleop-signaling-server` is a standalone shared-runtime binary; it does
not require GStreamer or camera dependencies. Keep it on `127.0.0.1:8765` and
place `deployments/caddy/Caddyfile` on the public 443 boundary. The Caddy route
serves API/WSS only and returns 404 for every other path, including any driving
page. See [deployments/caddy/README.md](deployments/caddy/README.md) for the
trusted-certificate deployment boundary and local internal-CA test recipe.
Run `mine-teleop-signaling-server --help` to list the supported startup options;
help/version exit without opening a listener, and unknown option names fail
closed instead of silently starting with defaults.
For multiple identities, pass `--config configs/signaling-server.2x2.dev.yaml`
or set `MINE_TELEOP_SIGNALING_CONFIG`. The YAML contains only driver/vehicle
IDs, allowlists, and secret file/environment references; it rejects duplicate
IDs, empty allowlists, unknown vehicles, missing secrets, and ambiguous secret
sources. Legacy `--driver-id`/`--vehicle-id` mode remains available for one
development pair but cannot be mixed with multi-identity mode.
On macOS, exercise a built signaling server and control client as two real
control processes against one multi-identity server with:

```bash
./scripts/test/check_macos_control_2x2.sh /path/to/cmake-build-directory
```

默认执行快速双车双控隔离门。要执行首轮 30 分钟稳定性门并保留 CSV、审计和进程日志：

```bash
MINE_TELEOP_SOAK_SECONDS=1800 \
MINE_TELEOP_KEEP_EVIDENCE=1 \
  ./scripts/test/check_macos_control_2x2.sh /path/to/cmake-build-directory
```

稳定性模式每 5 秒维持车辆/驾驶员心跳、触发控制租约续期，并采样三进程 RSS、
文件描述符和 2/2/2 健康计数；末段相对早期平均 RSS 默认不得增长超过 16 MiB，
总文件描述符不得增长超过 8。

The check uses dynamic loopback ports and temporary credentials, proves two
simultaneous WSS sessions and identity-correct control commands, rejects both
cross-vehicle requests without releasing either valid authority, verifies
secret-free correlated audit records, safely logs both drivers out, and removes
its temporary runtime directory.
Driver login failures are bounded in memory per configured account, with one
shared bucket for all unknown account names. The default policy locks login for
five minutes after five failures within one minute and returns HTTP 429 plus
`Retry-After`; tune it with `--login-max-failures`,
`--login-failure-window-ms`, and `--login-lockout-ms`. A second source-aware
guard covers both HTTP API requests and WSS handshakes. It defaults to 600
requests per source per minute, retains at most 4096 explicit sources plus one
shared overflow bucket, and trusts `X-Forwarded-For` only when the TCP peer is an
explicitly configured proxy IP. Tune it with `--api-rate-limit-requests`,
`--api-rate-limit-window-ms`, `--api-rate-limit-max-sources`, and
`--trusted-proxy-addresses`. The default trusted peers are loopback Caddy only;
the limiter is process-local and does not replace authenticated route quotas or
a shared multi-instance edge limiter. Signaling audit JSONL is serialized by a
dedicated mutex, closes the active slice every UTC hour, and keeps the most
recent seven days by default. The 64 MiB/5-file limit remains a within-hour
overflow guard; tune it with `--audit-log-max-bytes`, `--audit-log-files`, and
`--audit-log-retention-days`. Each service construction writes a UTC
`signaling_service_started` record, so restarts remain visible inside the
bounded retained window. `/health` returns an `alerts` array without identities:
active login lockouts and a live source-table overflow switch status from `ok`
to `degraded` until the corresponding window expires. A random
`service_instance_id` is shared by `/health` and every audit record for one
process lifetime, then changes on restart so background events remain
correlatable without pretending the ID is a durable host identity.
The native regression suite also performs a real same-port signaling process
restart: the old driver, session, and control credentials are rejected, the
long-lived control runtime drops stale authority, detects the new service
instance, and reauthenticates with its stored native credential. During a
transport outage it exposes only a stale-safe vehicle snapshot with every
vehicle forced offline and uncontrollable; it never restores the old session or
control authority. Only a newly created control token is accepted by the
vehicle receiver.

Use `MINE_TELEOP_MACOS_ARCH=x64` for an Intel build. A default cross-compiled
build records `runtime_tests_executed=no` and still requires runtime acceptance
on an Intel Mac or a Rosetta-enabled host. Passing `test` is rejected when the
target architecture cannot run on the current host.

## Self-contained Ubuntu x64 vehicle bundle

On an Apple Silicon Mac, build the complete Ubuntu 22.04 x86_64/amd64 vehicle
bundle without a previously accepted base archive:

```bash
scripts/build/build_macos_vehicle_from_scratch.sh
```

This path starts from a clean Ubuntu 22.04 amd64 container, installs the
distribution's GStreamer/WebRTC, VAAPI, NVCodec, and Aravis packages, builds
the small pinned OpenSSL libsrtp compatibility library and all current Mine
Teleop C++ targets from source, and collects the runtime dependency closure.
Pass `test` to compile the test targets, run CTest, and validate the exact final
archive. It avoids the unreliable
ARM-to-x86 QEMU build of the complete GStreamer source tree. The original
command automatically selects this path on Apple Silicon when no
`MINE_TELEOP_BASE_BUNDLE_ARCHIVE` is set:

```bash
scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

Build and run tests with:

```bash
scripts/build/build_cpp_ubuntu_bundle.sh test linux/amd64
```

Vehicle packages require the JYR010 chassis bridge and ChassisControl runtime.
When either ignored vendor binary is absent, the build entrypoint first runs
`scripts/build/prepare_chassis_runtime.sh`. By default it reads sibling
`../ChassisControl` sources and `../MinePilot/libchassis_control.so`; override
them with `MINE_TELEOP_CHASSIS_CONTROL_ROOT` and
`MINE_TELEOP_CHASSIS_CONTROL_LIBRARY`. The package build and final archive
checker both fail if either required `.so` is absent or has an unresolved
dependency.

The script emits
`dist/mine-teleop-vehicle-ubuntu22.04-x64-YYYYMMDD-HHMMSS.tar.gz`, its SHA-256
file. In `test` mode it runs `scripts/test/check_cpp_ubuntu_bundle.sh` against
that exact archive inside a fresh Ubuntu 22.04 amd64 container. The exported artifact contains
x86-64 ELF executables and shared libraries under `bin/` and `lib/`, plus the
field vehicle configuration, CA roots, Basler udev helper, protocol files,
README, and build proof. It carries GStreamer WebRTC/RTP/MP4 plugins, Intel's
VAAPI userspace driver, and the Ubuntu 22.04 dynamic loader/glibc. It does not
carry FFmpeg, Python, or credentials. The static `bin/mine-teleop-run` launcher
discovers the bundle location, configures the bundled loader/media paths, and
starts the configured C++ vehicle services.
The device token remains outside the package. NVIDIA's kernel
driver remains a hardware/OS prerequisite; the matching redistributable NVIDIA
userspace libraries must be copied into `lib/` for a field package.

On a native x86_64 Linux builder, `scripts/build/build_cpp_ubuntu_bundle.sh` retains
the pinned third-party source-build path. When only application code changed,
a checksum-verified accepted bundle can
provide the unchanged third-party media runtime while every native binary is
rebuilt from current source:

```bash
MINE_TELEOP_BASE_BUNDLE_ARCHIVE=/path/to/accepted-vehicle-bundle.tar.gz \
  scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

`BUILD-INFO.txt` records the base archive name and whether tests ran. Add
`test` to run the complete clean-container package gate.

For the chassis integration or licensed Hikrobot cameras, place redistributable
files under `vendor/chassis` or `vendor/mvs` before building. See
`vendor/README.md`. Basler USB3 Vision cameras use the pinned, library-only
Aravis/libusb build and do not require pylon. All bridge shared libraries are
carried in the bundle; the target x64 Ubuntu host does not need an SDK
installation or source checkout.

## Deployment

The industrial PC does not compile source. After extraction and protected
device-token provisioning, `./bin/mine-teleop-run` is the single foreground
vehicle entry point. The full cloud, vehicle, and control-client procedure is
in [docs/DEPLOY.md](docs/DEPLOY.md).

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

An active driver session renews its short control-authority lease through the
loopback runtime before one third of the remaining TTL elapses. Renewal is
authenticated with the current driver token, preserves the existing session
and DataChannel control token, and is audited without logging either token.
If browser/runtime refreshes stop, the signaling server still expires and
clears the authority at the last issued deadline.

## Development control plane

```bash
scripts/test/run_control_plane_docker_smoke.sh
scripts/deploy/run_control_plane_docker.sh
```

The smoke creates native signaling and console containers, grants one vehicle
session, verifies control-message isolation, and exchanges browser media codec
capabilities without using the removed per-frame upload path.

## Operational boundaries

- The default camera `backend: auto` preserves the existing selectors: ordinary
  V4L2 devices use native C++ `ioctl`/mmap/poll and must provide camera-native
  MJPEG, while `testsrc` and vendor bridge selectors keep their existing paths.
  Acquisition does not launch FFmpeg; the test source is generated and JPEG
  encoded in-process as before.
- CCG2-8M channels must opt in with `backend: ccg2`; see
  `configs/vehicle-agent.ccg2-8m.yaml`. This backend requests the driver's
  reported YUYV format, treats the actual packed bytes as UYVY, and sends the
  tightly packed raw frame directly into GStreamer before hardware encoding.
  Only this raw path inserts `videorate` when capture and output FPS differ;
  the legacy MJPEG pipeline is unchanged. CCG2 startup also requires the
  driver's returned `timeperframe` to exactly match the configured capture FPS.
  The negotiated V4L2 `1920x1080` is the application-visible frame; a board
  input status of `1920x1536` is diagnostic metadata, not a capture height.
  Error-flagged buffers are requeued without becoming fresh frames, while V4L2
  sequence/gap metadata remains visible in diagnostics. Kernel driver
  installation and board initialization remain deployment tasks. The CCG2
  support package creates stable `/dev/ccg2-channel-0` through
  `/dev/ccg2-channel-7` links from the XDMA channel index; use those links in
  configuration instead of enumeration-dependent `/dev/videoN` names. The CCG2
  Ubuntu 22.04 example selects Intel VAAPI first: the validated GStreamer
  1.20.3/RTX 2000 Ada host rejects NVCodec presets at runtime, while VAAPI
  successfully encodes the same raw input. Select NVENC first on this host only
  after upgrading to GStreamer 1.24 or newer and re-running the encoder probe.
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
