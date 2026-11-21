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
$ExpectedCurlLinkage = [string]$DependencyLock.curl.linkage

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
  $OutputDirectory = Join-Path $RepoRoot "dist"
} else {
  $OutputDirectory = Get-AbsolutePath -Path $OutputDirectory -BasePath $WorkingDirectory
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
  $BuildDirectory = Join-Path $RepoRoot "build\windows-cloud-$Architecture"
} else {
  $BuildDirectory = Get-AbsolutePath -Path $BuildDirectory -BasePath $WorkingDirectory
}

if ([string]::IsNullOrWhiteSpace($Triplet)) {
  $Triplet = $LockedTriplet
} elseif ($Triplet -ne $LockedTriplet) {
  throw "Windows cloud bundle triplet must match the dependency lock: expected $LockedTriplet, got $Triplet"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
  throw "VCPKG_ROOT is not set. Pass -VcpkgRoot or set the VCPKG_ROOT environment variable."
}
$VcpkgRoot = (Resolve-Path $VcpkgRoot).Path

if ($null -eq (Get-Command cmake -ErrorAction SilentlyContinue)) {
  throw "cmake is required but was not found in PATH. Install Visual Studio with the C++ build tools."
}

$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
  throw "vswhere.exe is required to locate Visual Studio C++ build tools"
}
$VisualStudioInstances = @(& $VsWhere -products '*' -format json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw "Cannot enumerate Visual Studio installations" }
$CmakeCapabilities = & cmake -E capabilities | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "Cannot enumerate CMake generators" }
$VisualStudioInstance = $null
$VisualStudioGenerator = $null
foreach ($instance in ($VisualStudioInstances | Sort-Object { [version]$_.installationVersion } -Descending)) {
  if (-not (Test-Path -LiteralPath (Join-Path $instance.installationPath "VC\Tools\MSVC") -PathType Container)) { continue }
  $major = ([version]$instance.installationVersion).Major
  $generator = @($CmakeCapabilities.generators | Where-Object { $_.name -like "Visual Studio $major *" })
  if ($generator.Count -eq 1) {
    $VisualStudioInstance = $instance.installationPath
    $VisualStudioGenerator = $generator[0].name
    break
  }
}
if ($null -eq $VisualStudioGenerator) {
  throw "No installed Visual Studio C++ instance has a matching CMake generator; update CMake or install the matching build tools"
}
Write-Host "==> Using $VisualStudioGenerator at $VisualStudioInstance"

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

$CurlPatch = Join-Path $RepoRoot "packaging\windows\patches\schannel-connect-only-progress.patch"
$CurlPatchHash = (Get-FileHash -LiteralPath $CurlPatch -Algorithm SHA256).Hash.ToLowerInvariant()
if ($CurlPatchHash -ne [string]$DependencyLock.curl.schannel_send_progress_patch_sha256) {
  throw "Schannel send-progress patch does not match the dependency lock"
}
$OverlayRoot = Join-Path $BuildDirectory "vcpkg-overlays"
$CurlOverlay = Join-Path $OverlayRoot "curl"
$InstalledRoot = Join-Path $BuildDirectory "vcpkg-installed"
New-Item -ItemType Directory -Path $CurlOverlay -Force | Out-Null
Copy-Item -Path (Join-Path $VcpkgRoot "ports\curl\*") -Destination $CurlOverlay -Recurse -Force
Copy-Item -LiteralPath $CurlPatch -Destination $CurlOverlay -Force
$OverlayPortfile = Join-Path $CurlOverlay "portfile.cmake"
$PortfileText = Get-Content -LiteralPath $OverlayPortfile -Raw
$PatchSection = [regex]'(?m)^    PATCHES\r?$'
if ($PatchSection.Matches($PortfileText).Count -ne 1) {
  throw "Pinned curl port has an unexpected patch list"
}
$PortfileText = $PatchSection.Replace($PortfileText, "    PATCHES`n        schannel-connect-only-progress.patch")
[IO.File]::WriteAllText($OverlayPortfile, $PortfileText)
$OverlayManifestPath = Join-Path $CurlOverlay "vcpkg.json"
$OverlayManifest = Get-Content -LiteralPath $OverlayManifestPath -Raw | ConvertFrom-Json
$OverlayManifest | Add-Member -NotePropertyName "port-version" -NotePropertyValue ([int]$DependencyLock.curl.overlay_revision) -Force
$OverlayManifest | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $OverlayManifestPath -Encoding utf8
Write-Host "==> Pinned Schannel progress overlay SHA-256: $CurlPatchHash"

if (-not $SkipDependencyInstall) {
  Write-Host "==> Installing Windows signaling-server dependencies for $Triplet"
  Invoke-CheckedCommand -Command $VcpkgExecutable -CommandArguments @(
    "install",
    "--overlay-ports=$OverlayRoot",
    "--x-install-root=$InstalledRoot",
    "curl:$Triplet",
    "openssl:$Triplet",
    "yaml-cpp:$Triplet",
    "nlohmann-json:$Triplet"
  )
}

$VcpkgPackageList = @(& $VcpkgExecutable "list" "--x-install-root=$InstalledRoot")
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
$ExpectedOverlayVersion = "$ExpectedCurlPortVersion#$([int]$DependencyLock.curl.overlay_revision)"
if ($CurlPortVersion -ne $ExpectedOverlayVersion) {
  throw "Windows bundle requires patched curl $ExpectedOverlayVersion, got $CurlPortVersion"
}

$CmakeArchitecture = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
$MsvcRuntime = if ($Triplet.EndsWith("-static")) {
  'MultiThreaded$<$<CONFIG:Debug>:Debug>'
} else {
  'MultiThreaded$<$<CONFIG:Debug>:Debug>DLL'
}
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null

Write-Host "==> Configuring Windows cloud signaling server (architecture=$Architecture, triplet=$Triplet)"
Invoke-CheckedCommand -Command "cmake" -CommandArguments @(
  "-U", "CURL_*",
  "-S", $RepoRoot,
  "-B", $BuildDirectory,
  "-G", $VisualStudioGenerator,
  "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioInstance",
  "-A", $CmakeArchitecture,
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
  "-DVCPKG_TARGET_TRIPLET=$Triplet",
  "-DVCPKG_OVERLAY_PORTS=$OverlayRoot",
  "-DVCPKG_INSTALLED_DIR=$InstalledRoot",
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=$MsvcRuntime",
  "-DVCPKG_APPLOCAL_DEPS=ON",
  "-DMINE_TELEOP_CURL_LINKAGE=$ExpectedCurlLinkage",
  "-DMINE_TELEOP_BUILD_CONTROL_CLIENT=OFF",
  "-DMINE_TELEOP_BUILD_SIGNALING_SERVER=ON",
  "-DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF",
  "-DMINE_TELEOP_BUILD_TESTS=OFF",
  "-DMINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS=OFF",
  "-DMINE_TELEOP_FETCH_MISSING_DEPS=OFF"
)

Write-Host "==> Building Windows cloud signaling server (jobs=$BuildJobs)"
Invoke-CheckedCommand -Command "cmake" -CommandArguments @(
  "--build", $BuildDirectory,
  "--config", "Release",
  "--target", "mine-teleop-signaling-server",
  "--parallel", "$BuildJobs"
)

$ConfigurationDirectory = Join-Path $BuildDirectory "Release"
$Executable = Join-Path $ConfigurationDirectory "mine-teleop-signaling-server.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  throw "The expected signaling executable was not produced at $Executable"
}

$UnexpectedCurlDlls = @(Get-ChildItem -LiteralPath $ConfigurationDirectory -Filter "*curl*.dll" -File)
if ($UnexpectedCurlDlls.Count -ne 0) {
  throw "curl must be linked statically, but the build produced: $($UnexpectedCurlDlls.Name -join ', ')"
}

Write-Host "==> Running signaling-server CLI and config-validation check"
Invoke-CheckedCommand -Command $Executable -CommandArguments @("--version")
Invoke-CheckedCommand -Command $Executable -CommandArguments @("--help")

$TempCheckDir = Join-Path ([IO.Path]::GetTempPath()) ("mine-teleop-win-cloud-check-" + [Guid]::NewGuid().ToString("N"))
$SmokeProcess = $null
try {
  $TempSecrets = Join-Path $TempCheckDir "secrets"
  New-Item -ItemType Directory -Path $TempSecrets -Force | Out-Null
  [IO.File]::WriteAllText((Join-Path $TempSecrets "driver-console-001.password"), "smoke-driver-password")
  [IO.File]::WriteAllText((Join-Path $TempSecrets "vehicle-001.token"), "smoke-vehicle-token")
  $TempConfig = Join-Path $TempCheckDir "signaling-server.lan.yaml"
  Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows-cloud\signaling-server.lan.yaml.example") `
    -Destination $TempConfig

  Invoke-CheckedCommand -Command $Executable -CommandArguments @(
    "--config", $TempConfig,
    "--host", "127.0.0.1",
    "--port", "8765",
    "--stun-urls", "stun:127.0.0.1:3478",
    "--validate-config"
  )

  if ($SmokeTest) {
    Write-Host "==> Running live loopback /health smoke test"
    $Listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $Listener.Start()
    $SmokePort = ([System.Net.IPEndPoint]$Listener.LocalEndpoint).Port
    $Listener.Stop()

    $SmokeAudit = Join-Path $TempCheckDir "signaling-audit.jsonl"
    $SmokeProcess = Start-Process -FilePath $Executable `
      -ArgumentList @(
        "--config", $TempConfig,
        "--host", "127.0.0.1",
        "--port", "$SmokePort",
        "--stun-urls", "stun:127.0.0.1:3478",
        "--audit-log", $SmokeAudit
      ) `
      -PassThru -WindowStyle Hidden

    $Healthy = $false
    for ($Attempt = 0; $Attempt -lt 30; $Attempt++) {
      Start-Sleep -Milliseconds 200
      if ($SmokeProcess.HasExited) {
        throw "Signaling server exited prematurely during smoke test (exit code $($SmokeProcess.ExitCode))"
      }
      try {
        $Health = Invoke-RestMethod -Uri "http://127.0.0.1:$SmokePort/health" -TimeoutSec 2
        if ($Health.status -eq "ok" -and -not [string]::IsNullOrWhiteSpace([string]$Health.native_control_capabilities)) {
          Write-Host "==> Smoke /health OK: capabilities=$($Health.native_control_capabilities)"
          $Healthy = $true
          break
        }
      } catch {}
    }
    if (-not $Healthy) {
      throw "Signaling server did not report healthy on http://127.0.0.1:$SmokePort/health"
    }
  }
} finally {
  if ($null -ne $SmokeProcess -and -not $SmokeProcess.HasExited) {
    Stop-Process -Id $SmokeProcess.Id -Force -ErrorAction SilentlyContinue
  }
  if (Test-Path -LiteralPath $TempCheckDir) {
    Remove-Item -LiteralPath $TempCheckDir -Recurse -Force -ErrorAction SilentlyContinue
  }
}

$Timestamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
$PackageName = "mine-teleop-cloud-windows-$Architecture-$Timestamp"
$PackageRoot = Join-Path $OutputDirectory $PackageName
$Archive = "$PackageRoot.zip"

if (Test-Path -LiteralPath $PackageRoot) {
  throw "Package directory already exists: $PackageRoot"
}
if (Test-Path -LiteralPath $Archive) {
  throw "Package archive already exists: $Archive"
}

New-Item -ItemType Directory -Path (Join-Path $PackageRoot "bin") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "config\secrets") -Force | Out-Null

Copy-Item -LiteralPath $Executable -Destination (Join-Path $PackageRoot "bin")
Get-ChildItem -LiteralPath $ConfigurationDirectory -Filter "*.dll" -File | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PackageRoot "bin")
}
if (-not $Triplet.EndsWith("-static")) {
  foreach ($RuntimeName in @("vcruntime140.dll", "msvcp140.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot "bin\$RuntimeName") -PathType Leaf)) {
      throw "Missing app-local compiler runtime: $RuntimeName"
    }
  }
}

Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows-cloud\signaling-server.lan.yaml.example") `
  -Destination (Join-Path $PackageRoot "config\signaling-server.lan.yaml")
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows-cloud\signaling-server.lan.yaml.example") `
  -Destination (Join-Path $PackageRoot "config\signaling-server.lan.yaml.example")
@(
  "Place your deployment secret files in this directory before starting run-lan-signaling.ps1:",
  "  driver-console-001.password  (driver login password)",
  "  vehicle-001.token            (must match the vehicle's config/device-token)"
) | Set-Content -LiteralPath (Join-Path $PackageRoot "config\secrets\PLACE-SECRETS-HERE.txt") -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows-cloud\run-lan-signaling.ps1") `
  -Destination (Join-Path $PackageRoot "run-lan-signaling.ps1")
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows-cloud\README.txt") `
  -Destination (Join-Path $PackageRoot "README.txt")
Copy-Item -LiteralPath $DependencyLockPath `
  -Destination (Join-Path $PackageRoot "DEPENDENCIES.lock.json")

$ReleaseVersion = if ([string]::IsNullOrWhiteSpace($env:MINE_TELEOP_RELEASE_VERSION)) {
  "0.2.0"
} else {
  $env:MINE_TELEOP_RELEASE_VERSION.Trim()
}
$SourceCommit = ((& git -C $RepoRoot rev-parse HEAD) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot read the MineTeleop source commit"
}
$SourceStatus = ((& git -C $RepoRoot status --short) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Cannot read the MineTeleop source status"
}
$SourceDirty = -not [string]::IsNullOrWhiteSpace($SourceStatus)
$RecordedSourceCommit = if ($SourceDirty) { "$SourceCommit-dirty" } else { $SourceCommit }

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
  role = "cloud-signaling"
  release_version = $ReleaseVersion
  source_commit = $SourceCommit
  source_dirty = $SourceDirty
  vcpkg_commit = $ActualVcpkgCommit
  triplet = $Triplet
  curl = [ordered]@{
    port_version = $CurlPortVersion
    linkage = $ExpectedCurlLinkage
    schannel_send_progress_patch_sha256 = $CurlPatchHash
    overlay_revision = [int]$DependencyLock.curl.overlay_revision
  }
  runtime_files = @($RuntimeFiles)
}
$DependencyManifest |
  ConvertTo-Json -Depth 6 |
  Set-Content -LiteralPath (Join-Path $PackageRoot "DEPENDENCIES.json") -Encoding UTF8

$TestsExecuted = if ($SmokeTest) {
  "cli-version,cli-help,validate-config,loopback-health-smoke"
} else {
  "cli-version,cli-help,validate-config"
}
@(
  "target_platform=windows",
  "target_architecture=$Architecture",
  "role=cloud-signaling",
  "dependency_triplet=$Triplet",
  "release_version=$ReleaseVersion",
  "source_commit=$RecordedSourceCommit",
  "vcpkg_commit=$ActualVcpkgCommit",
  "libcurl_port_version=$CurlPortVersion",
  "libcurl_linkage=$ExpectedCurlLinkage",
  "libcurl_schannel_send_progress_patch_sha256=$CurlPatchHash",
  "runtime_tests_executed=$TestsExecuted",
  "dependency_manifest=DEPENDENCIES.json",
  "built_at_utc=$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
) | Set-Content -LiteralPath (Join-Path $PackageRoot "BUILD-INFO.txt") -Encoding UTF8

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Write-Host "==> Creating Windows cloud signaling bundle"
Compress-Archive -LiteralPath $PackageRoot -DestinationPath $Archive -CompressionLevel Optimal
$ArchiveHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$ArchiveHash  $([IO.Path]::GetFileName($Archive))" | `
  Set-Content -LiteralPath "$Archive.sha256" -Encoding ASCII

Write-Output "windows_cloud_bundle=$Archive"
Write-Output "windows_cloud_bundle_sha256=$Archive.sha256"
Write-Output "windows_cloud_bundle_root=$PackageRoot"
