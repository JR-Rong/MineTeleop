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

  bin\            Windows executable and any app-local runtime DLLs
  config\         local baseline and field-tested three-machine driver YAML
  protocol\       protocol-v1 interoperability vectors
  run-control.ps1 one-command PowerShell launcher
  BUILD-INFO.txt  target architecture, dependency triplet, and test status

This package is a build artifact until it passes Windows-host acceptance. At a
minimum, verify startup, loopback-only binding, browser opening, occupied-port
failure, HTTPS/WSS certificate validation, login, vehicle selection, media and
DataChannel behavior, emergency stop, safe release, and shutdown port cleanup.
