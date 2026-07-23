# Ubuntu vehicle + cloud + Mac live delivery

This is the live field handoff for the first three-machine path. Windows and
Ubuntu control clients are intentionally deferred. Physical chassis actuation
is not part of this gate: the vehicle configuration uses the mock adapter and
`max_speed_kph: 0`.

## Current result

| Gate | State | Live evidence |
|---|---|---|
| Direct cloud signaling | passed | Mac and Ubuntu vehicle connect directly to `teleop-field.internal:6000`, resolved inside each native process to `60.205.213.254`; no SSH, SOCKS, FRP, or hosts-file dependency is present. |
| Mac login and vehicle selection | passed | Pinned-CA TLS login lists `vehicle-001` online and controllable. |
| UVC camera on Mac | passed | `front_uvc`, 1280x720, 30 FPS, 0% packet loss; the browser video clock advances continuously. |
| Mac command received on vehicle | passed | In final direct session `session-000003`, the unreliable DataChannel delivered 721 accepted and 0 rejected zero-state commands at about 20 Hz. |
| Safe disconnect | passed | Safe logout closed the DataChannel and the vehicle emitted `safety_action=local_full_stop` with the accepted/rejected counters. |
| Cross-host time gate | passed | The final direct run reported 5 ms time-sync uncertainty, below the required 25 ms maximum. |
| Session recovery | passed | After `session-000008` closed, the same vehicle service PID returned to its wait loop and established `session-000009` without a vehicle restart. |
| Basler camera on Mac | passed | USB access is `root:video 660`; Aravis identifies `acA1300-200uc / 25192546`, a 1280x720 JPEG capture passes, and the Mac renders the live track. |
| Simultaneous dual-camera playback | passed | Final direct session `session-000003` renders `front_uvc` and `basler_25192546` at 1280x720 and about 30 FPS with 0% packet loss; both video clocks advance. |
| Dual-camera 30-minute soak | passed | `session-000001` ran for 1,801 seconds with both 1280x720 tracks at 30 FPS. The same vehicle PID and Basler child remained alive for all 31 resource samples; late RSS averaged only 57.6 KiB above the post-fill early average. |
| Cloud restart recovery | passed | A transient Caddy 502 is now retried with a local full stop. The patched vehicle service kept PID `558069`, re-registered against the new signaling instance, and established another two-track/DataChannel session. |

## Current topology

| Role | Endpoint | State |
|---|---|---|
| Cloud signaling | `https://teleop-field.internal:6000`, app-local resolve to `60.205.213.254`, Caddy to `127.0.0.1:8765` | active; pinned internal CA |
| Cloud compatibility entry | `https://60-205-213-254.sslip.io` on public 443 | active; not used by the final field session |
| Cloud TURN | `60.205.213.254:3478` UDP/TCP, relay UDP 49152-49200 | active; configured but not selected by the accepted session |
| Ubuntu vehicle | native runtime in `/home/cz/mine-teleop`, direct TCP to cloud port 6000 | active |
| Mac control | native arm64 process on `127.0.0.1:8080`, direct TCP to cloud port 6000 | active |

The accepted media and control path was classified as STUN. The native clients
use `cloud.resolve` only inside libcurl and verify
`teleop-field.internal` against the packaged field root CA. Process and socket
inspection confirmed that neither client had proxy environment variables or a
connection to the retired local SOCKS port. SSH remains maintenance access only
and is not in the normal driving path.

## Start and stop the three ends

### Cloud

Install the units and drop-ins once as described in
`deployments/systemd/README.md`, then use the target as the single lifecycle
entry:

```bash
sudo systemctl start mine-teleop-cloud.target
sudo systemctl restart mine-teleop-cloud.target
sudo systemctl stop mine-teleop-cloud.target
sudo systemctl --no-pager --full status \
  mine-teleop-cloud.target \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service
```

The target groups four independent daemons; it does not merge them into one
process. This preserves component-specific restart behavior and logs while
giving operators one start/stop command.

### Ubuntu vehicle

```bash
tar -xzf mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz
cd mine-teleop-vehicle-ubuntu22.04-x64-*
printf '%s\n' '<device-token-from-secret-store>' > config/device-token
chmod 600 config/device-token
./bin/mine-teleop-run
```

The vehicle stays in the foreground and stops with `Ctrl-C`; the delivery does
not install a vehicle systemd unit.

### macOS control

```bash
tar -xzf mine-teleop-control-macos-arm64-*.tar.gz
cd mine-teleop-control-macos-arm64-*
./run-control.command --config config/driver-console.three-machine.yaml
```

Enter the driver password in the loopback page or provide it through
`MINE_TELEOP_DRIVER_PASSWORD`; do not write it into YAML or shell history.

## `all` versus forced TURN

`ice_transport_policy: all` gathers direct, STUN, and TURN candidates and lets
ICE choose the best available route. This is the production default and the
accepted session selected STUN. `ice_transport_policy: relay` gathers only TURN
candidates. It is used to prove the fallback path or to operate on a network
that intentionally forbids peer-to-peer connectivity; it normally adds relay
bandwidth cost and latency. Both modes keep WebRTC media and DataChannel
encrypted.

## Credential transport patch

Follow-up driver and vehicle GET/WSS authentication now uses
`X-Mine-Teleop-Driver-Token` and `X-Mine-Teleop-Device-Token` headers rather
than URL query parameters. This prevents bearer values from being copied into
proxy access logs, URL history, and URL-bearing error messages. Login and
vehicle registration remain TLS-protected POST JSON. The server accepts query
credentials only as a temporary old-client fallback; deploy all three updated
ends and rotate any credential that may previously have appeared in URL logs
before removing the fallback.

## Vehicle artifact

The current field bundle is:

```text
dist/mine-teleop-vehicle-ubuntu22.04-x64-20260723-065537.tar.gz
SHA-256 0fd8031acc5c251087f865d5549e937fe2a3f703efd3fc21d5af08753fcdcef8
```

It contains the native x64 runtime, GStreamer 1.28.5 WebRTC/V4L2/NVENC/VA
plugins, Aravis/libusb, the pinned OpenSSL SRTP library, CA roots, the two-camera
field configuration, the bounded vehicle telemetry history, and the Basler udev
installer. It contains no device token. Every native binary was rebuilt from
the current source after 11/11 amd64 CTest; the unchanged third-party media
runtime was reused from the checksum-verified previously accepted archive to
avoid an unrelated QEMU/GCC internal compiler crash while rebuilding
GStreamer. The exact new archive was copied into a fresh Ubuntu 22.04 amd64
container: its launcher, signaling server, control helper and Aravis bridge
started, the two-camera field YAML passed `config-check`, and the WebRTC/V4L2/
NVENC/VA/SRTP/SCTP/DTLS/RTP/H.264 plugin checks passed. Both field CA copies
were present and a credential filename scan was empty.

The current Mac arm64 control bundle is:

```text
dist/mine-teleop-control-macos-arm64-20260723-064607.tar.gz
SHA-256 6d6f610b7d435383bf9872e745aafafc26539f89d2cb5b430d8fb8c33ad9c38d
```

It is ad-hoc signed, includes the field CA and
`config/driver-console.three-machine.yaml`, and passes all three native tests
plus archive checksum, Mach-O architecture, dependency, JavaScript syntax,
extracted-runtime, and loopback health gates. Its README gives the direct
one-command field launch and contains no obsolete tunnel instructions.

The real camera mapping is:

- `front_uvc`: `/dev/v4l/by-id/usb-Generic_USB_Camera_200901010001-video-index0`
- `basler_25192546`: `basler:serial=25192546`

## Who decides the number of video feeds

The vehicle decides. Every item under `cameras` with `enabled: true` creates one
WebRTC video track. The cloud forwards authentication and signaling but does
not add or remove tracks; the Mac renders the tracks advertised by the vehicle
offer. The current field YAML enables `front_uvc` and `basler_25192546`, so the
accepted session has two feeds. Runtime track selection from the control page
is not implemented; changing the count currently requires a vehicle-config
change and a media-session restart.

## Final direct-path evidence

The authoritative no-tunnel run is `session-000003`:

- H.264/NVENC, STUN selected, TURN configured but unused.
- `front_uvc`: 30 FPS, 0% loss, approximately 10.7 ms.
- `basler_25192546`: 30 FPS, 0% loss, approximately 19.7 ms.
- Both browser video elements were 1280x720, `readyState=4`, unpaused, and
  advanced from about 34.426 to 36.026 seconds in a 1.6-second sample.
- Control RTT was 2 ms and time-sync uncertainty was 5 ms.
- Vehicle close evidence recorded 721 accepted commands, 0 rejected commands,
  and `safety_action=local_full_stop`.
- An earlier direct session remained connected for about 84 seconds, beyond
  the obsolete 60-second failure point, with both tracks at 30 FPS and 0% loss.

The redacted handoff record and checksum manifest are under
`dist/evidence/2026-07-23/`; `shasum -a 256 -c SHA256SUMS` validates both
records and both final archives.

## Basler access installed

The one-time rule was installed with:

```bash
ssh -t czcz 'sudo /home/cz/mine-teleop/scripts/setup_basler_usb_access.sh cz'
```

The script installs only `99-mine-teleop-basler.rules`, grants the standard
`video` group access to Basler vendor `2676`, reloads udev, and adds `cz` to the
`video` group. The live post-install state is `root:video 660`; a new SSH login
contains the `video` group.

Verify before starting the dual-camera runtime:

```bash
ssh czcz 'cd /home/cz/mine-teleop && LD_LIBRARY_PATH="$PWD/lib" ./bin/mine-teleop-aravis-camera --list --json'
```

The live result is `device_count: 1`, model `acA1300-200uc`, serial `25192546`,
type `usb3vision`. A captured frame is a 175,259-byte baseline JPEG at 1280x720.

## Live UVC evidence

The first accepted live session (`session-000007`) showed:

- WebRTC state `connected`, H.264/NVENC, STUN, TURN configured but unused.
- `front_uvc` at 1280x720 and about 30 FPS with zero packet loss.
- Browser playback `readyState=4`, not paused; `currentTime` advanced from
  `146.201` to `147.700` in 1.5 seconds.
- More than 16,000 consecutive vehicle-accepted control messages before the
  safe-exit test; command state was forced zero because no Gamepad was present.

The recovery run then established `session-000008`, performed safe logout, and
established `session-000009` without restarting the Ubuntu service. The second
video element check advanced from `4.317` to `5.559` in 1.2 seconds at 1280x720.

The dual-camera run established `session-000010`. Both tracks reported about
30-31 FPS and 0% packet loss. DOM playback checks found two 1280x720 elements,
both `readyState=4` and unpaused; their clocks advanced together from `26.512`
to `28.112` in 1.6 seconds. H.264/NVENC, STUN, TURN configured/unused, control
authority, and the zero-input DataChannel were all active.

That first dual run deliberately stopped after 1,058,682 ms when RSS growth was
correlated with the vehicle's unbounded 10 Hz telemetry vector. It had encoded
31,753 UVC and 31,748 Basler frames at about 29.99 FPS and accepted 21,006
zero-state commands before safe logout. The service now retains only the newest
1,024 telemetry samples while preserving a separate total count; a 2,049-sample
boundary test and all 21 C++ core tests pass. `session-000011` is the bounded
build used for the authoritative 30-minute soak.

The default 30-minute driver bearer lifetime was one setup interval shorter
than a possible 30-minute media session. The field service now uses a 60-minute
driver bearer for this gate while retaining the five-minute renewable control
lease. The first attempt safely expired at about 29 minutes 50 seconds and
produced `local_full_stop`; it was not counted as the passing soak.

The authoritative replacement session ran from `2026-07-22T16:40:32Z` through
`17:10:33Z`. Both browser video clocks reached `1855.023` seconds. The final
sample reported Basler/front UVC at 30/30 FPS, about 0.003%/0.009% packet loss,
and 27.46/20.47 ms estimated latency. RSS after telemetry retention filled was
193,740 KiB at sample 3 and 193,812 KiB at sample 30; descriptors ranged from
67 to 72 and ended at 70. Safe logout closed the DataChannel and again emitted
`local_full_stop`.

After the soak, a controlled cloud restart exposed a transient Caddy HTTP 502.
Service mode now treats 5xx responses as signaling transport failures, performs
a local full stop, and retries after one second. Live recovery kept the same
vehicle PID, restored online presence, then delivered two 30 FPS tracks and 465
accepted/0 rejected zero-state commands in a post-restart session.

## Remaining acceptance

The requested Ubuntu vehicle + cloud + Mac media/control path is accepted. The
broader taskbook still requires:

1. Forced-TURN media/DataChannel evidence through the public security-group
   path.
2. Public two-vehicle/two-control concurrency and the 8-hour stability matrix.
3. Windows and Ubuntu control packages and their clean-host runtime acceptance.
4. Per-camera disconnect/recovery, congestion, and capture-to-display timestamp
   validation.
5. Real CAN adapter, braking-distance, physical ESTOP, and on-site safety
   acceptance; the current field configuration deliberately remains
   `vehicle_adapter.type=mock` with `max_speed_kph: 0`.
