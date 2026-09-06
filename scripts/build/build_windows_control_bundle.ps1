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
$DependencyLockPath = Join-Path $RepoRoot "packaging\windows\dependencies.lock.json"
if (-not (Test-Path -LiteralPath $DependencyLockPath -PathType Leaf)) {
  throw "Windows dependency lock was not found at $DependencyLockPath"
}
$DependencyLock = Get-Content -LiteralPath $DependencyLockPath -Raw | ConvertFrom-Json
$ExpectedVcpkgCommit = [string]$DependencyLock.vcpkg_commit
$LockedTripletProperty = $DependencyLock.triplets.PSObject.Properties[$Architecture]
if ($null -eq $LockedTripletProperty) {
  throw "Windows dependency lock has no triplet for architecture $Architecture"
}
$LockedTriplet = [string]$LockedTripletProperty.Value
$ExpectedCurlPortVersion = [string]$DependencyLock.curl.port_version
$ExpectedCurlRuntimeVersion = [string]$DependencyLock.curl.runtime_version
$ExpectedCurlTlsBackend = [string]$DependencyLock.curl.tls_backend
$ExpectedCurlLinkage = [string]$DependencyLock.curl.linkage

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
  $Triplet = $LockedTriplet
} elseif ($Triplet -ne $LockedTriplet) {
  throw "Windows bundle triplet must match the dependency lock: expected $LockedTriplet, got $Triplet"
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
if ($null -eq (Get-Command git -ErrorAction SilentlyContinue)) {
  throw "git is required to verify the pinned vcpkg checkout"
}
$ActualVcpkgCommit = ((& git -C $VcpkgRoot rev-parse HEAD) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot read the vcpkg commit from $VcpkgRoot"
}
if ($ActualVcpkgCommit -ne $ExpectedVcpkgCommit) {
  throw "Windows dependency lock requires vcpkg $ExpectedVcpkgCommit, got $ActualVcpkgCommit"
}
$VcpkgTrackedStatus = ((& git -C $VcpkgRoot status --short --untracked-files=no) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot verify the pinned vcpkg checkout"
}
if (-not [string]::IsNullOrWhiteSpace($VcpkgTrackedStatus)) {
  throw "The pinned vcpkg checkout has tracked modifications; refusing a non-reproducible package"
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

$VcpkgPackageList = @(& $VcpkgExecutable "list")
if ($LASTEXITCODE -ne 0) {
  throw "Cannot list installed vcpkg dependencies"
}
$TripletPattern = [Regex]::Escape($Triplet)
$CurlPackageLine = @(
  $VcpkgPackageList |
    Where-Object { $_ -match "^curl(?:\[core\])?:${TripletPattern}\s+" } |
    Select-Object -First 1
)
if ($CurlPackageLine.Count -ne 1) {
  throw "Cannot determine the installed curl port version for $Triplet"
}
$CurlPortVersion = (($CurlPackageLine[0] -split "\s+")[1])
if (($CurlPortVersion -split "#")[0] -ne $ExpectedCurlPortVersion) {
  throw "Windows dependency lock requires curl port $ExpectedCurlPortVersion, got $CurlPortVersion"
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
  "-DMINE_TELEOP_CURL_LINKAGE=$ExpectedCurlLinkage",
  "-DMINE_TELEOP_BUILD_CONTROL_CLIENT=ON",
  "-DMINE_TELEOP_BUILD_SIGNALING_SERVER=OFF",
  "-DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF",
  "-DMINE_TELEOP_BUILD_TESTS=OFF",
  "-DMINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS=ON",
  "-DMINE_TELEOP_FETCH_MISSING_DEPS=OFF"
)

Write-Host "==> Building Windows control client and portable safety test (jobs=$BuildJobs)"
Invoke-CheckedCommand -Command "cmake" -CommandArguments @(
  "--build", $BuildDirectory,
  "--config", "Release",
  "--target", "mine-teleop-control", "mine-teleop-native-control-intent-tests",
  "--parallel", "$BuildJobs"
)

Write-Host "==> Running portable native control safety test (Release)"
Invoke-CheckedCommand -Command "ctest" -CommandArguments @(
  "--test-dir", $BuildDirectory,
  "-C", "Release",
  "--output-on-failure",
  "--no-tests=error",
  "-R", "^mine-teleop-native-control-intent-tests$"
)

$ConfigurationDirectory = Join-Path $BuildDirectory "Release"
$Executable = Join-Path $ConfigurationDirectory "mine-teleop-control.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  throw "The expected control executable was not produced at $Executable"
}

$UnexpectedCurlDlls = @(Get-ChildItem -LiteralPath $ConfigurationDirectory -Filter "*curl*.dll" -File)
if ($UnexpectedCurlDlls.Count -ne 0) {
  throw "curl must be linked statically, but the build produced: $($UnexpectedCurlDlls.Name -join ', ')"
}

$DependencyInfoText = ((& $Executable "--dependency-info") | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Control-client dependency inspection failed with exit code $LASTEXITCODE"
}
try {
  $DependencyInfo = $DependencyInfoText | ConvertFrom-Json
} catch {
  throw "Control-client dependency inspection returned invalid JSON: $DependencyInfoText"
}
$ActualCurlCompileVersion = [string]$DependencyInfo.curl_compile_version
$ActualCurlRuntimeVersion = [string]$DependencyInfo.curl_runtime_version
$ActualCurlTlsBackend = [string]$DependencyInfo.curl_tls_backend
$ActualCurlLinkage = [string]$DependencyInfo.curl_linkage
if ($ActualCurlCompileVersion -ne $ExpectedCurlRuntimeVersion -or
    $ActualCurlRuntimeVersion -ne $ExpectedCurlRuntimeVersion) {
  throw "Windows dependency lock requires curl $ExpectedCurlRuntimeVersion, got compile=$ActualCurlCompileVersion runtime=$ActualCurlRuntimeVersion"
}
if (-not $ActualCurlTlsBackend.StartsWith($ExpectedCurlTlsBackend, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Windows dependency lock requires curl TLS backend $ExpectedCurlTlsBackend, got $ActualCurlTlsBackend"
}
if ($ActualCurlLinkage -ne $ExpectedCurlLinkage) {
  throw "Windows dependency lock requires curl linkage $ExpectedCurlLinkage, got $ActualCurlLinkage"
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
Copy-Item -LiteralPath $DependencyLockPath `
  -Destination (Join-Path $PackageRoot "DEPENDENCIES.lock.json")

$SourceCommit = ((& git -C $RepoRoot rev-parse HEAD) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot read the MineTeleop source commit"
}
$SourceStatus = ((& git -C $RepoRoot status --short) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot read the MineTeleop source status"
}
$RuntimeFiles = @(
  Get-ChildItem -LiteralPath (Join-Path $PackageRoot "bin") -File |
    Sort-Object Name |
    ForEach-Object {
      [ordered]@{
        path = "bin/$($_.Name)"
        size_bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
      }
    }
)
$RuntimeFiles |
  ForEach-Object { "$($_.sha256)  $($_.path)" } |
  Set-Content -LiteralPath (Join-Path $PackageRoot "RUNTIME-SHA256.txt") -Encoding ASCII
$VcpkgPackageList |
  Set-Content -LiteralPath (Join-Path $PackageRoot "THIRD-PARTY-VERSIONS.txt") -Encoding UTF8
$DependencyManifest = [ordered]@{
  schema_version = 1
  source_commit = $SourceCommit
  source_dirty = -not [string]::IsNullOrWhiteSpace($SourceStatus)
  vcpkg_commit = $ActualVcpkgCommit
  triplet = $Triplet
  curl = [ordered]@{
    port_version = $CurlPortVersion
    compile_version = $ActualCurlCompileVersion
    runtime_version = $ActualCurlRuntimeVersion
    tls_backend = $ActualCurlTlsBackend
    linkage = $ActualCurlLinkage
  }
  runtime_files = @($RuntimeFiles)
}
$DependencyManifest |
  ConvertTo-Json -Depth 6 |
  Set-Content -LiteralPath (Join-Path $PackageRoot "DEPENDENCIES.json") -Encoding UTF8

$TestsExecuted = if ($SmokeTest) {
  "portable-native-control-intent,help-smoke"
} else {
  "portable-native-control-intent"
}
@(
  "target_platform=windows",
  "target_architecture=$Architecture",
  "dependency_triplet=$Triplet",
  "source_commit=$SourceCommit",
  "vcpkg_commit=$ActualVcpkgCommit",
  "libcurl_port_version=$CurlPortVersion",
  "libcurl_compile_version=$ActualCurlCompileVersion",
  "libcurl_runtime_version=$ActualCurlRuntimeVersion",
  "libcurl_tls_backend=$ActualCurlTlsBackend",
  "libcurl_linkage=$ActualCurlLinkage",
  "native_tests_built=yes",
  "runtime_tests_executed=$TestsExecuted",
  "dependency_manifest=DEPENDENCIES.json",
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
