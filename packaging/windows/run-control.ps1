[CmdletBinding()]
param(
  [string]$Config = "",

  [ValidateRange(0, 65535)]
  [int]$Port = 8080,

  [string]$VehicleId = "",

  [string]$SignalingUrl = "",

  [ValidateSet("all", "relay")]
  [string]$IceTransportPolicy = "all",

  [switch]$NoOpenBrowser,

  [switch]$Help,

  [switch]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Executable = Join-Path $PSScriptRoot "bin\mine-teleop-control.exe"
$DefaultConfig = Join-Path $PSScriptRoot "config\driver-console.yaml"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  throw "Control executable was not found at $Executable"
}
if (-not (Test-Path -LiteralPath $DefaultConfig -PathType Leaf)) {
  throw "Default control configuration was not found at $DefaultConfig"
}

if ([string]::IsNullOrWhiteSpace($Config)) {
  $Config = $DefaultConfig
} elseif (-not [IO.Path]::IsPathRooted($Config)) {
  $Config = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Config))
}

$CommandArguments = @("--config", $Config, "--port", "$Port")
if (-not [string]::IsNullOrWhiteSpace($VehicleId)) {
  $CommandArguments += @("--vehicle-id", $VehicleId)
}
if (-not [string]::IsNullOrWhiteSpace($SignalingUrl)) {
  $CommandArguments += @("--signaling-url", $SignalingUrl)
}
if ($PSBoundParameters.ContainsKey("IceTransportPolicy")) {
  $CommandArguments += @("--ice-transport-policy", $IceTransportPolicy)
}
if ($NoOpenBrowser) {
  $CommandArguments += "--no-open-browser"
}
if ($Help) {
  $CommandArguments += "--help"
}
if ($Version) {
  $CommandArguments += "--version"
}

& $Executable @CommandArguments
exit $LASTEXITCODE
