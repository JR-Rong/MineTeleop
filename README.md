# Mine Teleop

Mine Teleop is now a native C++20 runtime. Vehicle media targets Ubuntu 22.04;
the separated browser-based control client now builds natively on macOS arm64.
Production control, signaling, driver-console, vehicle-media, recorder/uploader,
and camera bridge entry points do not require Python.

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

scripts/check.sh
```

The build runs the native CTest suite, configuration validation, a deterministic
safety timeout loop, authenticated driver-to-vehicle signaling, a native C++
test-source frame path, and atomic uploader checks.

### macOS cloud bundle

Build and test the Ubuntu 22.04 x86_64 cloud package from an Intel or Apple
Silicon Mac with Docker Desktop:

```bash
scripts/build_macos_cloud_bundle.sh
```

This uses a dedicated signaling-only Dockerfile and does not build GStreamer,
camera bridges, or the vehicle runtime. It emits
`dist/mine-teleop-cloud-ubuntu22.04-x64-YYYYMMDD-HHMMSS.tar.gz` plus a SHA-256
file. The final package is self-contained for the native signaling binary and
also carries the systemd, Caddy, HAProxy, and coturn deployment assets.

After uploading and extracting the archive on Ubuntu 22.04 x86_64, deploy all
four cloud services with the package-local script:

```bash
sudo ./deploy-cloud.sh \
  --signaling-config /secure/staging/signaling-server.yaml \
  --identity-secrets-dir /secure/staging/identity-secrets \
  --turn-secret-file /secure/staging/turn-static-auth.secret \
  --turn-realm 60-205-213-254.sslip.io \
  --turn-host 60-205-213-254.sslip.io \
  --caddy-config deployments/caddy/Caddyfile.three-machine \
  --haproxy-config deployments/haproxy/haproxy.three-machine.cfg
```

The bundled three-machine proxy files contain the current field addresses.
Inspect and edit copies before selecting them for another server. The deployer
preserves configuration unless a replacement is explicitly supplied, backs up
replaced files, validates all three application/proxy configurations, enables
`mine-teleop-cloud.target`, and requires the loopback health check to pass.
Use `sudo ./deploy-cloud.sh --no-start` to install without starting.

### macOS control client

Build, test, sign, and package the native control client without GStreamer or
camera dependencies:

```bash
scripts/build_macos_control_bundle.sh
```

The generated `dist/mine-teleop-control-macos-arm64-*.tar.gz` contains the
executable, shared YAML, protocol-v1 vectors, CA bundle, build proof, and
`run-control.command`. It uses the macOS Security/CommonCrypto system APIs and
does not require Homebrew libraries. The process binds only to `127.0.0.1`, opens
the default browser, and emits a clear error if the configured port is busy.
Every build also runs `scripts/check_macos_control_bundle.sh` against the final
archive. All targets verify checksum, signature, architecture, dependencies, and
contents; native builds additionally verify extracted startup, loopback and
port-conflict behavior, page syntax, the redacted local event log, and clean
shutdown. A cross-compiled build stops after static checks and remains build-only.
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
./scripts/check_macos_control_2x2.sh /path/to/cmake-build-directory
```

默认执行快速双车双控隔离门。要执行首轮 30 分钟稳定性门并保留 CSV、审计和进程日志：

```bash
MINE_TELEOP_SOAK_SECONDS=1800 \
MINE_TELEOP_KEEP_EVIDENCE=1 \
  ./scripts/check_macos_control_2x2.sh /path/to/cmake-build-directory
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

Use `MINE_TELEOP_MACOS_ARCH=x64` for an Intel build. When cross-compiling on an
Apple Silicon host without Rosetta, add `MINE_TELEOP_SKIP_RUN_TESTS=1`; the
resulting `BUILD-INFO.txt` explicitly records that it is build-only and still
requires runtime acceptance on an Intel Mac or a Rosetta-enabled host.

## Self-contained Ubuntu x64 bundle

Build the Ubuntu 22.04 x86_64/amd64 bundle from any Docker-capable host:

```bash
scripts/build_cpp_ubuntu_bundle.sh linux/amd64
```

The script emits
`dist/mine-teleop-vehicle-ubuntu22.04-x64-YYYYMMDD-HHMMSS.tar.gz`, its SHA-256
file, and runs `scripts/check_cpp_ubuntu_bundle.sh` against that exact archive
inside a fresh Ubuntu 22.04 amd64 container. The exported artifact contains
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

When only application code changed, a checksum-verified accepted bundle can
provide the unchanged third-party media runtime while every native binary is
rebuilt and retested from current source:

```bash
MINE_TELEOP_BASE_BUNDLE_ARCHIVE=/path/to/accepted-vehicle-bundle.tar.gz \
  scripts/build_cpp_ubuntu_bundle.sh linux/amd64
```

`BUILD-INFO.txt` records the base archive name, and the final archive still
runs the complete clean-container package gate.

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

An active driver session renews its short control-authority lease through the
loopback runtime before one third of the remaining TTL elapses. Renewal is
authenticated with the current driver token, preserves the existing session
and DataChannel control token, and is audited without logging either token.
If browser/runtime refreshes stop, the signaling server still expires and
clears the authority at the last issued deadline.

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
