param([string]$ModulePath=(Join-Path $PSScriptRoot '../../packaging/windows/performance-profile.psm1'))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Windows PowerShell 5.1 compiles a real native argument sink. No power changes,
# elevation, scheduled tasks or vehicle connection are performed by these tests.
Import-Module $ModulePath -Force
$before=Get-PowerInspection|ConvertTo-Json -Depth 6 -Compress
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('mine-power-test-'+[Guid]::NewGuid())
New-Item -ItemType Directory -Path $temporary|Out-Null
try {
  $exe=Join-Path $temporary 'argument sink.exe'
  Add-Type -OutputAssembly $exe -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Text;
class ArgumentSink {
 public static void Main(string[] args) {
  Console.WriteLine(args.Length);
  foreach(string arg in args) Console.WriteLine(Convert.ToBase64String(Encoding.UTF8.GetBytes(arg)));
 }
}
'@
  $values=@('', 'plain', 'two words', 'a"b', 'C:\space path\', 'slash\"quote', '$(throw "injected")', '`backtick', "apostrophe's", ([string][char]0x4e2d+[char]0x6587))
  $info=[Diagnostics.ProcessStartInfo]::new()
  $info.FileName=$exe;$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true
  $info.Arguments=ConvertTo-PowerCommandLine $values
  $p=[Diagnostics.Process]::Start($info)
  $lines=$p.StandardOutput.ReadToEnd() -split '\r?\n'
  $p.WaitForExit()
  if($p.ExitCode -ne 0 -or [int]$lines[0] -ne $values.Count){throw 'Native argument count mismatch'}
  for($i=0;$i -lt $values.Count;$i++) {
    if([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($lines[$i+1])) -cne $values[$i]){throw "Native argument mismatch at $i"}
  }
  $script=Join-Path $temporary "elevation's parameter sink.ps1"
  $output=Join-Path $temporary 'parameters.json'
  @'
param([string]$Target,[string[]]$ClientArguments,[switch]$Restore)
@{Target=$Target;ClientArguments=$ClientArguments;Restore=[bool]$Restore}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $env:MINE_POWER_TEST_OUTPUT -Encoding UTF8
exit 7
'@|Set-Content -LiteralPath $script -Encoding UTF8
  $command=New-PowerElevationCommand $script @{Target='control';ClientArguments=$values;Restore=[Management.Automation.SwitchParameter]::new($true)}
  $info.FileName=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $info.EnvironmentVariables['MINE_POWER_TEST_OUTPUT']=$output
  $info.Arguments='-NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
  $p=[Diagnostics.Process]::Start($info);$null=$p.StandardOutput.ReadToEnd();$p.WaitForExit()
  if($p.ExitCode -ne 7){throw 'Elevation command did not propagate child exit code'}
  $actual=Get-Content -LiteralPath $output -Raw|ConvertFrom-Json
  if($actual.Target -ne 'control' -or !$actual.Restore -or $actual.ClientArguments.Count -ne $values.Count){throw 'Elevation parameter shape mismatch'}
  for($i=0;$i -lt $values.Count;$i++){if($actual.ClientArguments[$i] -cne $values[$i]){throw "Elevation argument mismatch at $i"}}
  $command=New-PowerElevationCommand $script @{}
  $info.Arguments='-NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
  $p=[Diagnostics.Process]::Start($info);$null=$p.StandardOutput.ReadToEnd();$p.WaitForExit()
  $actual=Get-Content -LiteralPath $output -Raw|ConvertFrom-Json
  if($p.ExitCode -ne 7 -or $actual.Restore -or $actual.Target -or $null -ne $actual.ClientArguments){throw 'Default double-click elevation payload mismatch'}
  'throw "expected startup failure"'|Set-Content -LiteralPath $script -Encoding UTF8
  $p=[Diagnostics.Process]::Start($info);$null=$p.StandardOutput.ReadToEnd();$p.WaitForExit()
  if($p.ExitCode -ne 1){throw 'Terminating startup error was reported as success'}
  if((Get-PowerInspection|ConvertTo-Json -Depth 6 -Compress) -cne $before){throw 'Read-only tests changed power settings'}
  Write-Output 'PASS: native argv, UAC payload/exit-code round trip, read-only inspection'
} finally {Remove-Item -LiteralPath $temporary -Recurse -Force}
