Mine Teleop Control for Windows
===============================

Requirements:

  - 64-bit Windows 10 or Windows 11
  - A current Microsoft Edge, Chrome, or Firefox browser

Start from PowerShell:

  powershell -ExecutionPolicy Bypass -File .\run-control.ps1

The program binds only to 127.0.0.1 and opens the local control page in the
default browser. The bundled config is a local-development baseline. To use
the field-tested three-machine endpoint, select its config explicitly:

  powershell -ExecutionPolicy Bypass -File .\run-control.ps1 -Config config\driver-console.three-machine.yaml

The three-machine config resolves teleop-field.internal inside the application
and verifies it with config\mine-teleop-field-root.crt. It does not require a
hosts-file change, SSH, SOCKS, or FRP.

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
