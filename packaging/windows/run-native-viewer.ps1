[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$GStreamerRoot,
  [string]$ViewerPath = (Join-Path $PSScriptRoot 'mine-teleop-windows-viewer.exe'),
  [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $GStreamerRoot).Path
$viewer = (Resolve-Path -LiteralPath $ViewerPath).Path
$bin = Join-Path $root 'bin'
$inspector = Join-Path $bin 'gst-inspect-1.0.exe'
if (-not (Test-Path -LiteralPath $inspector -PathType Leaf)) { throw 'GStreamer gst-inspect-1.0.exe is missing' }
$env:PATH = "$bin;$env:PATH"
$env:GST_PLUGIN_SYSTEM_PATH = Join-Path $root 'lib\gstreamer-1.0'
$scanner = Join-Path $root 'libexec\gstreamer-1.0\gst-plugin-scanner.exe'
if (Test-Path -LiteralPath $scanner -PathType Leaf) { $env:GST_PLUGIN_SCANNER = $scanner }
foreach ($element in @('webrtcbin', 'nicesrc', 'rtph264depay', 'h264parse', 'avdec_h264', 'glimagesink')) {
  & $inspector $element *> $null
  if ($LASTEXITCODE -ne 0) { throw "Required GStreamer element unavailable: $element" }
}
if ($CheckOnly) { Write-Output 'Native viewer and required GStreamer elements are available'; exit 0 }
$process = Start-Process -FilePath $viewer -PassThru -Wait
exit $process.ExitCode
