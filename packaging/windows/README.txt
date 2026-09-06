Mine Teleop Control for Windows
===============================

Requirements:

  - 64-bit Windows 10 or Windows 11
  - A current Microsoft Edge, Chrome, or Firefox browser

Start the three-machine control client:

  1. Open the bin folder.
  2. Double-click mine-teleop-control.exe.
  3. Enter the driver credentials in the browser page that opens.

The executable loads config\driver-console.yaml automatically. That bundled
default connects to the field three-machine signaling endpoint, resolves it
inside the application, and verifies it with the bundled private CA.

PowerShell remains available for diagnostics or overrides:

  powershell -ExecutionPolicy Bypass -File .\run-control.ps1

The program binds only to 127.0.0.1 and opens the local control page in the
default browser. To run the local-development endpoint instead, select its
config explicitly:

  powershell -ExecutionPolicy Bypass -File .\run-control.ps1 -Config config\driver-console.local-development.yaml

The default three-machine path does not require a hosts-file change, SSH,
SOCKS, or FRP. On Windows, certificate-chain and hostname verification remain
enabled; the app accepts an unknown revocation status only when a custom CA
bundle is explicitly configured and that private CA has no CRL/OCSP endpoint.

Do not store the driver password in this directory, README, or YAML. Enter it
in the local control page or provide it temporarily through the
MINE_TELEOP_DRIVER_PASSWORD process environment variable.

Contents:

  bin\            Windows executable; curl and third-party libraries are static
  config\         local baseline and field-tested three-machine driver YAML
  protocol\       protocol-v1 interoperability vectors
  run-control.ps1 one-command PowerShell launcher
  BUILD-INFO.txt  source, architecture, curl/TLS versions, and build status
  DEPENDENCIES.json exact linked curl provenance and runtime file hashes
  DEPENDENCIES.lock.json reproducible vcpkg/curl build inputs
  THIRD-PARTY-VERSIONS.txt complete vcpkg dependency version list
  RUNTIME-SHA256.txt executable and app-local runtime hashes

The package does not load libcurl from Windows or PATH. curl is statically
linked into mine-teleop-control.exe at build time. Run this command to inspect
the version and TLS backend actually compiled into the executable:

  .\bin\mine-teleop-control.exe --dependency-info

The Windows build runs a Release native-control safety test before packaging.
It verifies lease-expiry neutralization with gear preservation, invalidation
without stale-throttle replay, the fresh-neutral interlock, and sticky ESTOP.
BUILD-INFO.txt records that test execution, but the package remains a build
artifact until it passes Windows-host and end-to-end acceptance.

At a minimum, verify startup, loopback-only binding, browser opening,
occupied-port failure, HTTPS/WSS certificate validation, login, vehicle
selection, and media. Verify that the browser submits only its latest input
intent, while the native process sends commands at 20 Hz over its dedicated
WSS connection, the signaling mailbox keeps only the latest command with a
150 ms TTL, and the vehicle receives it over a separate control-only WSS while
its watchdog ticks independently every 50 ms. After browser focus loss, hiding,
or freezing, the intent lease must expire while native transport continues with
neutral actuation marked stale. The vehicle accepts only exact-zero stale
actuation as a safe native-link heartbeat, rejects every stale non-zero command,
and returning to the page must not restore old throttle before a fresh neutral
input. A real native-process or WSS packet gap still triggers the vehicle
watchdog. The DataChannel remains the fail-closed session-profile
and VCU-handshake gate and carries status only. Also verify emergency stop,
safe release, and shutdown port cleanup.
