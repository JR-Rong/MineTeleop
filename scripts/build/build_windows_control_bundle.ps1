[CmdletBinding()]
param(
  [ValidateSet("x64", "arm64")]
  [string]$Architecture = "x64",

  [string]$Triplet = "",

  [string]$OutputDirectory = "",

  [string]$BuildDirectory = "",

  [string]$VcpkgRoot = $env:VCPKG_ROOT,

  [ValidateRange(1, 256)]
  [int]$BuildJobs = [Environment]::ProcessorCount,

  [switch]$SkipDependencyInstall,

  [switch]$SmokeTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-AbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$BasePath
  )

  if ([IO.Path]::IsPathRooted($Path)) {
    return [IO.Path]::GetFullPath($Path)
  }
  return [IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Invoke-CheckedCommand {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Command,

    [Parameter(Mandatory = $true)]
    [string[]]$CommandArguments
  )

  & $Command @CommandArguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code ${LASTEXITCODE}: $Command"
  }
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$WorkingDirectory = (Get-Location).Path

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
  $OutputDirectory = Join-Path $RepoRoot "dist"
} else {
  $OutputDirectory = Get-AbsolutePath -Path $OutputDirectory -BasePath $WorkingDirectory
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
  $BuildDirectory = Join-Path $RepoRoot "build\windows-control-$Architecture"
} else {
  $BuildDirectory = Get-AbsolutePath -Path $BuildDirectory -BasePath $WorkingDirectory
}

if ([string]::IsNullOrWhiteSpace($Triplet)) {
  $Triplet = "$Architecture-windows"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
  throw "VCPKG_ROOT is not set. Pass -VcpkgRoot or set the VCPKG_ROOT environment variable."
}
$VcpkgRoot = (Resolve-Path $VcpkgRoot).Path

if ($null -eq (Get-Command cmake -ErrorAction SilentlyContinue)) {
  throw "cmake is required but was not found in PATH. Install Visual Studio 2022 with Desktop development with C++."
}

$VcpkgExecutable = Join-Path $VcpkgRoot "vcpkg.exe"
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $VcpkgExecutable -PathType Leaf)) {
  throw "vcpkg.exe was not found at $VcpkgExecutable"
}
if (-not (Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf)) {
  throw "The vcpkg CMake toolchain was not found at $VcpkgToolchain"
}

if (-not $SkipDependencyInstall) {
  Write-Host "==> Installing control-client dependencies for $Triplet"
  Invoke-CheckedCommand -Command $VcpkgExecutable -CommandArguments @(
    "install",
    "curl:$Triplet",
    "openssl:$Triplet",
    "yaml-cpp:$Triplet",
    "nlohmann-json:$Triplet"
  )
}

$CmakeArchitecture = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null

Write-Host "==> Configuring Windows control client (architecture=$Architecture, triplet=$Triplet)"
Invoke-CheckedCommand -Command "cmake" -CommandArguments @(
  "-S", $RepoRoot,
  "-B", $BuildDirectory,
  "-G", "Visual Studio 17 2022",
  "-A", $CmakeArchitecture,
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
  "-DVCPKG_TARGET_TRIPLET=$Triplet",
  "-DVCPKG_APPLOCAL_DEPS=ON",
  "-DMINE_TELEOP_BUILD_CONTROL_CLIENT=ON",
  "-DMINE_TELEOP_BUILD_SIGNALING_SERVER=OFF",
  "-DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF",
  "-DMINE_TELEOP_BUILD_TESTS=OFF",
  "-DMINE_TELEOP_FETCH_MISSING_DEPS=OFF"
)

Write-Host "==> Building Windows control client (jobs=$BuildJobs)"
Invoke-CheckedCommand -Command "cmake" -CommandArguments @(
  "--build", $BuildDirectory,
  "--config", "Release",
  "--target", "mine-teleop-control",
  "--parallel", "$BuildJobs"
)

$ConfigurationDirectory = Join-Path $BuildDirectory "Release"
$Executable = Join-Path $ConfigurationDirectory "mine-teleop-control.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  throw "The expected control executable was not produced at $Executable"
}

if ($SmokeTest) {
  Write-Host "==> Running control-client help smoke test"
  Invoke-CheckedCommand -Command $Executable -CommandArguments @("--help")
}

$Timestamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
$PackageName = "mine-teleop-control-windows-$Architecture-$Timestamp"
$PackageRoot = Join-Path $OutputDirectory $PackageName
$Archive = "$PackageRoot.zip"

if (Test-Path -LiteralPath $PackageRoot) {
  throw "Package directory already exists: $PackageRoot"
}
if (Test-Path -LiteralPath $Archive) {
  throw "Package archive already exists: $Archive"
}

New-Item -ItemType Directory -Path (Join-Path $PackageRoot "bin") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "config") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "protocol\v1") -Force | Out-Null

Copy-Item -LiteralPath $Executable -Destination (Join-Path $PackageRoot "bin")
Get-ChildItem -LiteralPath $ConfigurationDirectory -Filter "*.dll" -File | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PackageRoot "bin")
}
Copy-Item -LiteralPath (Join-Path $RepoRoot "configs\driver-console.three-machine.dev.yaml") `
  -Destination (Join-Path $PackageRoot "config\driver-console.yaml")
Copy-Item -LiteralPath (Join-Path $RepoRoot "configs\driver-console.three-machine.dev.yaml") `
  -Destination (Join-Path $PackageRoot "config\driver-console.three-machine.yaml")
Copy-Item -LiteralPath (Join-Path $RepoRoot "configs\driver-console.dev.yaml") `
  -Destination (Join-Path $PackageRoot "config\driver-console.local-development.yaml")
Copy-Item -LiteralPath (Join-Path $RepoRoot "configs\mine-teleop-field-root.crt") `
  -Destination (Join-Path $PackageRoot "config\mine-teleop-field-root.crt")
Copy-Item -Path (Join-Path $RepoRoot "protocol\v1\*") `
  -Destination (Join-Path $PackageRoot "protocol\v1") -Recurse
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows\run-control.ps1") `
  -Destination (Join-Path $PackageRoot "run-control.ps1")
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows\README.txt") `
  -Destination (Join-Path $PackageRoot "README.txt")

$TestsExecuted = if ($SmokeTest) { "help-smoke-only" } else { "no" }
@(
  "target_platform=windows",
  "target_architecture=$Architecture",
  "dependency_triplet=$Triplet",
  "runtime_tests_executed=$TestsExecuted",
  "built_at_utc=$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
) | Set-Content -LiteralPath (Join-Path $PackageRoot "BUILD-INFO.txt") -Encoding UTF8

Write-Host "==> Creating Windows control bundle"
Compress-Archive -LiteralPath $PackageRoot -DestinationPath $Archive -CompressionLevel Optimal
$ArchiveHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$ArchiveHash  $([IO.Path]::GetFileName($Archive))" | `
  Set-Content -LiteralPath "$Archive.sha256" -Encoding ASCII

Write-Output "windows_control_bundle=$Archive"
Write-Output "windows_control_bundle_sha256=$Archive.sha256"
Write-Output "windows_control_bundle_root=$PackageRoot"
