[CmdletBinding()]
param(
  [ValidateRange(1, 65535)]
  [int]$Port = 8765,

  [string]$HostAddress = "0.0.0.0",

  [string]$Config = "",

  [string]$DriverId = "",
  [string]$DriverPassword = "",
  [string]$VehicleId = "",
  [string]$DeviceToken = "",

  [string]$StunUrls = "stun:127.0.0.1:3478",

  [switch]$ValidateConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Executable = Join-Path $PSScriptRoot "bin\mine-teleop-signaling-server.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  throw "Signaling executable not found: $Executable"
}

$LogDir = Join-Path $PSScriptRoot "logs"
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
$AuditLog = Join-Path $LogDir "signaling-audit.jsonl"

$UseInlineIdentity = (
  -not [string]::IsNullOrWhiteSpace($DriverId) -or
  -not [string]::IsNullOrWhiteSpace($DriverPassword) -or
  -not [string]::IsNullOrWhiteSpace($VehicleId) -or
  -not [string]::IsNullOrWhiteSpace($DeviceToken)
)

$ArgsList = @(
  "--host", $HostAddress,
  "--port", "$Port",
  "--stun-urls", $StunUrls,
  "--driver-token-ttl-ms", "3600000",
  "--control-token-ttl-ms", "300000",
  "--vehicle-heartbeat-ms", "15000",
  "--driver-heartbeat-ms", "15000",
  "--api-rate-limit-requests", "6000",
  "--audit-log", $AuditLog
)

if ($HostAddress -ne "127.0.0.1" -and $HostAddress -ne "::1" -and $HostAddress -ne "localhost") {
  $ArgsList += "--allow-insecure-nonloopback-dev"
}

if ($UseInlineIdentity) {
  if (-not [string]::IsNullOrWhiteSpace($DriverId)) { $ArgsList += @("--driver-id", $DriverId) }
  if (-not [string]::IsNullOrWhiteSpace($DriverPassword)) { $ArgsList += @("--driver-password", $DriverPassword) }
  if (-not [string]::IsNullOrWhiteSpace($VehicleId)) { $ArgsList += @("--vehicle-id", $VehicleId) }
  if (-not [string]::IsNullOrWhiteSpace($DeviceToken)) { $ArgsList += @("--device-token", $DeviceToken) }
} else {
  if ([string]::IsNullOrWhiteSpace($Config)) {
    $Config = Join-Path $PSScriptRoot "config\signaling-server.lan.yaml"
  } elseif (-not [IO.Path]::IsPathRooted($Config)) {
    $Config = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Config))
  }
  if (-not (Test-Path -LiteralPath $Config -PathType Leaf)) {
    throw "Signaling config not found: $Config"
  }
  $ArgsList += @("--config", $Config)
}

if ($ValidateConfig) {
  $ArgsList += "--validate-config"
}

Write-Host "=== Mine Teleop Windows LAN Signaling Server ==="
Write-Host "Listening on: ${HostAddress}:${Port}"
try {
  $LanIps = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object -ExpandProperty IPAddress -Unique)
  if ($LanIps.Count -gt 0) {
    Write-Host "Detected LAN IPv4: $($LanIps -join ', ')"
    Write-Host "Vehicle signaling_url example: ws://$($LanIps[0]):${Port}/signaling"
  }
} catch {}
Write-Host "Local control client signaling_url: ws://127.0.0.1:${Port}/signaling (MUST be ws://, not wss://)"
Write-Host "Press Ctrl+C to stop."
Write-Host ""

& $Executable @ArgsList
exit $LASTEXITCODE
