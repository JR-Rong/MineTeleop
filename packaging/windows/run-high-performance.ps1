[CmdletBinding()]
param(
  [ValidateSet('auto','control','desktop','native')][string]$Target='auto',
  [string[]]$ClientArguments=@(),
  [string]$GStreamerRoot='',
  [string]$ViewerPath='',
  [switch]$Inspect,
  [switch]$Restore,
  [string]$WatchSession='',
  [int]$WatchProcess=0,
  [string]$WatchStarted=''
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'performance-profile.psm1') -Force
$powershell=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
function Encode-Command([string]$Value){return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Value))}

if($Inspect){Get-PowerInspection|ConvertTo-Json -Depth 6;exit 0}
if(-not (Test-PowerAdministrator)){
  $command=New-PowerElevationCommand $PSCommandPath $PSBoundParameters
  try{$p=Start-Process -FilePath $powershell -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','RemoteSigned','-EncodedCommand',(Encode-Command $command)) -Wait -PassThru;exit $p.ExitCode}
  catch{Write-Error ('Administrator approval was cancelled or failed: '+$_.Exception.Message);exit 1}
}

if($WatchSession){
  $lock=Enter-PowerLock
  try{
    $file=Get-PowerStatePath
    $s=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json
    if($s.Session -ne $WatchSession -or $s.ParentId -ne $WatchProcess -or $s.ParentStarted -ne $WatchStarted){throw 'Watcher identity mismatch'}
    $s.WatcherReady=$true;Write-PowerState $s
  }finally{$lock.Dispose()}
  while(Test-PowerProcess $WatchProcess $WatchStarted){Start-Sleep -Seconds 1}
  $lock=Enter-PowerLock
  try{
    if(Test-Path -LiteralPath $file){
      $s=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json
      if($s.Session -eq $WatchSession){
        if($s.ChildId -gt 0 -and (Test-PowerProcess $s.ChildId $s.ChildStarted)){Stop-Process -Id $s.ChildId -Force}
        Restore-PowerProfile $WatchSession
      }
    }
  }finally{$lock.Dispose()}
  exit 0
}
if($Restore){
  $lock=Enter-PowerLock
  try{
    $file=Get-PowerStatePath
    if(Test-Path -LiteralPath $file){$s=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json;if(Test-PowerProcess $s.ParentId $s.ParentStarted){throw 'Close the managed launcher before restoring its power profile'};Restore-PowerProfile $s.Session}
    Write-Host 'Original power settings restored; no active managed profile remains.'
  }finally{$lock.Dispose()}
  exit 0
}

if($Target -eq 'auto'){$Target=if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'MineTeleop.exe')){'desktop'}elseif(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bin\mine-teleop-control.exe')){'control'}else{'native'}}
$exe=switch($Target){
  'desktop'{Join-Path $PSScriptRoot 'MineTeleop.exe'}
  'control'{Join-Path $PSScriptRoot 'bin\mine-teleop-control.exe'}
  'native'{
    if(-not $GStreamerRoot){$GStreamerRoot=$env:GSTREAMER_1_0_ROOT_MSVC_X86_64}
    if(-not $GStreamerRoot){throw 'Native viewer requires -GStreamerRoot'}
    if(-not $ViewerPath){$ViewerPath=Join-Path $PSScriptRoot 'mine-teleop-windows-viewer.exe'}
    & (Join-Path $PSScriptRoot 'run-native-viewer.ps1') -GStreamerRoot $GStreamerRoot -ViewerPath $ViewerPath -PrepareOnly
    $ViewerPath
  }
}
$exe=(Resolve-Path -LiteralPath $exe).Path
if(@(Get-Process -ErrorAction SilentlyContinue|Where-Object {$_.Path -eq $exe}).Count){throw 'Close the existing client before using its managed high-performance launcher'}
$state=$null;$child=$null;$code=1
try{
  $lock=Enter-PowerLock
  try{$state=Start-PowerProfile}finally{$lock.Dispose()}
  $script=$PSCommandPath.Replace("'","''")
  $watch="& '$script' -WatchSession '$($state.Session)' -WatchProcess $PID -WatchStarted '$($state.ParentStarted)'"
  $journalDirectory=Split-Path (Get-PowerStatePath)
  # The watcher must not share a caller's redirected pipes or console lifetime.
  Start-Process -FilePath $powershell -WindowStyle Hidden -RedirectStandardOutput (Join-Path $journalDirectory 'watcher.out.log') -RedirectStandardError (Join-Path $journalDirectory 'watcher.err.log') -ArgumentList @('-NoProfile','-ExecutionPolicy','RemoteSigned','-EncodedCommand',(Encode-Command $watch))|Out-Null
  $ready=$false
  for($i=0;$i -lt 100;$i++){$s=Get-Content -LiteralPath (Get-PowerStatePath) -Raw|ConvertFrom-Json;if($s.Session -eq $state.Session -and $s.WatcherReady){$state=$s;$ready=$true;break};Start-Sleep -Milliseconds 100}
  if(-not $ready){throw 'Independent restoration watcher did not start; power settings were not changed'}
  $lock=Enter-PowerLock
  try{
    $actual=Enable-PowerProfile $state
    Write-Host 'High Performance is active. Keep this launcher open; closing it stops the client and restores power settings.'
    Write-Output (@{event='windows_performance_profile_ready';settings=$actual}|ConvertTo-Json -Depth 6 -Compress)
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$exe;$info.WorkingDirectory=$PSScriptRoot;$info.UseShellExecute=$false
    $info.Arguments=ConvertTo-PowerCommandLine $ClientArguments
    $child=[Diagnostics.Process]::Start($info)
    if($null -eq $child){throw 'Client did not start'}
    $state.ChildId=$child.Id;$state.ChildStarted=$child.StartTime.ToUniversalTime().Ticks.ToString();Write-PowerState $state
  }finally{$lock.Dispose()}
  $child.WaitForExit();$code=$child.ExitCode
  Write-Output (@{event='windows_performance_client_exited';exit_code=$code}|ConvertTo-Json -Compress)
}finally{
  if($child -and !$child.HasExited){$child.Kill();$child.WaitForExit()}
  if($state){$lock=Enter-PowerLock;try{Restore-PowerProfile $state.Session;Write-Host 'Original Windows power settings restored.'}finally{$lock.Dispose()}}
}
exit $code
