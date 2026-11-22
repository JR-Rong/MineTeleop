Mine Teleop Cloud Signaling Server for Windows (Offline LAN Deployment)
=======================================================================

Requirements:

  - 64-bit Windows 10 or Windows 11 (x64)
  - Isolated LAN where vehicle and control host can reach each other directly

Quick start (isolated offline LAN):

  1. Prepare credentials in config\secrets\ (never commit real secrets):
       config\secrets\driver-console-001.password  (driver login password)
       config\secrets\vehicle-001.token            (exact match of vehicle's config/device-token)

     Adjust config\signaling-server.lan.yaml if your driver or vehicle IDs differ.

  2. Allow inbound TCP 8765 in Windows Defender Firewall (Administrator PowerShell, once):
       New-NetFirewallRule -DisplayName "MineTeleop LAN Signaling 8765" `
         -Direction Inbound -Protocol TCP -LocalPort 8765 -Action Allow

  3. Validate configuration and start the server:
       powershell -ExecutionPolicy Bypass -File .\run-lan-signaling.ps1 -ValidateConfig
       powershell -ExecutionPolicy Bypass -File .\run-lan-signaling.ps1

  4. Configure the Windows control client on the SAME PC (loopback plain WS):
       cloud:
         signaling_url: ws://127.0.0.1:8765/signaling
         ice_transport_policy: all
     Remove or comment out cloud.auth_url (if https://), cloud.resolve, and cloud.ca_bundle.
     IMPORTANT: Use ws://127.0.0.1:8765/signaling (NOT wss:// and NOT localhost).
     Sending TLS (wss://) to a plain HTTP/WS listener causes a 500 ms time-sync timeout.

  5. Configure the Ubuntu vehicle (replace 192.168.1.55 with this PC's LAN IPv4):
       cloud:
         signaling_url: ws://192.168.1.55:8765/signaling
         auth_url: http://192.168.1.55:8765/auth
         device_token_file: device-token
         ice_transport_policy: all
     Remove or comment out cloud.resolve and cloud.ca_bundle.

  6. Verify from the vehicle before launching mine-teleop-run:
       curl -fsS http://192.168.1.55:8765/health

Security boundary:

  - run-lan-signaling.ps1 uses --allow-insecure-nonloopback-dev for plain HTTP/WS
    inside an isolated offline LAN. Do NOT expose port 8765 to public or untrusted
    networks. For public or untrusted networks, terminate TLS/WSS behind a reverse
    proxy (e.g. Caddy) and bind mine-teleop-signaling-server to 127.0.0.1 only.

Contents:

  bin\            mine-teleop-signaling-server.exe and app-local MSVC runtime DLLs
  config\         signaling-server.lan.yaml and signaling-server.lan.yaml.example
  run-lan-signaling.ps1
  README.txt
  BUILD-INFO.txt
  DEPENDENCIES.json
  DEPENDENCIES.lock.json
  THIRD-PARTY-VERSIONS.txt
  RUNTIME-SHA256.txt
