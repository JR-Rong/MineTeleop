Mine Teleop Control for macOS
=============================

Start:

  ./run-control.command

The program binds only to 127.0.0.1 and opens the local control page in the
default browser. To override the signaling service without editing YAML:

  ./run-control.command --signaling-url wss://signal.example.com/signaling

The bundled config is a local-development baseline. Replace its cloud URLs
with the deployment HTTPS/WSS endpoint before field use. Do not put passwords
or long-lived device credentials in this directory; provide the driver password
through MINE_TELEOP_DRIVER_PASSWORD or the future platform credential store.

This delivery also contains the field-tested three-machine endpoint as
config/driver-console.three-machine.yaml. It connects directly to the cloud
TLS/WSS listener with an application-local address override and pinned field
CA; it does not require SSH, SOCKS, FRP, or a system hosts-file change:

  ./run-control.command --config config/driver-console.three-machine.yaml

Never place the driver password in this README or in the YAML file. Use
MINE_TELEOP_DRIVER_PASSWORD or enter it in the local control page.

ICE policy:

  cloud.ice_transport_policy: all gathers host, STUN, and TURN candidates and
  lets ICE choose the best route. relay gathers only TURN relay candidates and
  should be used for a forced-TURN acceptance test. Both modes use encrypted
  WebRTC media/DataChannel transport; relay normally adds cloud bandwidth cost
  and latency.

Credential transport:

  Login remains an HTTPS POST. Subsequent driver bearer authentication is sent
  in X-Mine-Teleop-Driver-Token HTTP/WSS headers, not in URL query strings.
  The server keeps query fallback only for migration from older clients.

Contents:

  bin/            macOS executable
  lib/            reserved for bundled non-system libraries
  config/         local baseline and field-tested three-machine driver YAML
  certs/          CA certificate bundle
  protocol/       protocol-v1 interoperability vectors
  BUILD-INFO.txt  target architecture and runtime-test status

Local logs:

  Browser UTC events are written to .local/logs/control-browser-events.jsonl.
  The bundled config limits each file to 2 MiB and retains the current file
  plus two numbered backups. Password, token, secret, and credential fields
  are redacted before writing. These logs are local diagnostics, not a
  replacement for server audit or vehicle safety records.

Current tested platform: macOS arm64. H.264/H.265 and the real DataChannel
still require a live vehicle media session for field acceptance.

Browser support policy for this lightweight package:

  - Safari: the current and immediately previous stable major release.
  - Chrome or Edge: the current and immediately previous stable major release.
  - Firefox: the current and immediately previous stable major release; H.264
    is the required compatibility path.

The browser must provide WebRTC Unified Plan, RTCDataChannel, getStats(), and
the Gamepad API. H.265 is capability-detected and is never assumed. Pin and
record the exact browser build during field acceptance.
