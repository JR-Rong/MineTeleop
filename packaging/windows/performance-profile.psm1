Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'

function Initialize-PowerApi {
  if(-not [Environment]::Is64BitProcess){throw 'Use 64-bit Windows PowerShell for the 64-bit client'}
  if ('MineTeleop.PowerProfile' -as [type]) { return }
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
namespace MineTeleop {
 public static class PowerProfile {
  [DllImport("powrprof.dll")] static extern uint PowerGetActiveScheme(IntPtr root,out IntPtr value);
  [DllImport("powrprof.dll")] static extern uint PowerSetActiveScheme(IntPtr root,ref Guid value);
  [DllImport("powrprof.dll")] static extern uint PowerDeleteScheme(IntPtr root,ref Guid value);
  [DllImport("powrprof.dll")] static extern uint PowerWriteACValueIndex(IntPtr root,ref Guid plan,ref Guid subgroup,ref Guid setting,uint value);
  [DllImport("powrprof.dll")] static extern uint PowerWriteDCValueIndex(IntPtr root,ref Guid plan,ref Guid subgroup,ref Guid setting,uint value);
  [DllImport("powrprof.dll")] static extern uint PowerReadACValueIndex(IntPtr root,ref Guid plan,ref Guid subgroup,ref Guid setting,out uint value);
  [DllImport("powrprof.dll")] static extern uint PowerReadDCValueIndex(IntPtr root,ref Guid plan,ref Guid subgroup,ref Guid setting,out uint value);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr value);
  [StructLayout(LayoutKind.Explicit,Size=24)] struct ReasonUnion {[FieldOffset(0)]public IntPtr Text;}
  [StructLayout(LayoutKind.Sequential)] struct Reason {public uint Version,Flags;public ReasonUnion Data;}
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr PowerCreateRequest(ref Reason reason);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool PowerSetRequest(IntPtr handle,int type);
  [DllImport("kernel32.dll")] static extern bool PowerClearRequest(IntPtr handle,int type);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [StructLayout(LayoutKind.Sequential)] struct PowerStatus {public byte AC,Battery,Percent,EnergySaver;public uint Life,FullLife;}
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetSystemPowerStatus(out PowerStatus status);
  static IntPtr awake=IntPtr.Zero;
  static void Check(uint error){if(error!=0)throw new Win32Exception((int)error);}
  public static Guid Active(){IntPtr p;Check(PowerGetActiveScheme(IntPtr.Zero,out p));try{return (Guid)Marshal.PtrToStructure(p,typeof(Guid));}finally{LocalFree(p);}}
  public static void Activate(Guid plan){Check(PowerSetActiveScheme(IntPtr.Zero,ref plan));}
  public static void Delete(Guid plan){uint e=PowerDeleteScheme(IntPtr.Zero,ref plan);if(e!=2)Check(e);}
  static Guid wifi=new Guid("19cbb8fa-5279-450e-9fac-8a3d5fedd0c1"),saving=new Guid("12bbebe6-58d6-4636-95bb-3217ef867c1a");
  public static void WifiMaximum(Guid plan){Check(PowerWriteACValueIndex(IntPtr.Zero,ref plan,ref wifi,ref saving,0));Check(PowerWriteDCValueIndex(IntPtr.Zero,ref plan,ref wifi,ref saving,0));}
  public static uint WifiIndex(Guid plan,bool ac){uint value;Check(ac?PowerReadACValueIndex(IntPtr.Zero,ref plan,ref wifi,ref saving,out value):PowerReadDCValueIndex(IntPtr.Zero,ref plan,ref wifi,ref saving,out value));return value;}
  static Guid energy=new Guid("de830923-a562-41af-a086-e3a2c6bad2da"),threshold=new Guid("e69653ca-cf7f-4f05-aa73-cb833fa90ad4");
  public static uint? EnergyThreshold(Guid plan,bool ac){uint value;uint e=ac?PowerReadACValueIndex(IntPtr.Zero,ref plan,ref energy,ref threshold,out value):PowerReadDCValueIndex(IntPtr.Zero,ref plan,ref energy,ref threshold,out value);return e==0?(uint?)value:null;}
  public static void DisableAutomaticEnergySaver(Guid plan){Check(PowerWriteACValueIndex(IntPtr.Zero,ref plan,ref energy,ref threshold,0));Check(PowerWriteDCValueIndex(IntPtr.Zero,ref plan,ref energy,ref threshold,0));}
  public static bool EnergySaverActive(){PowerStatus status;if(!GetSystemPowerStatus(out status))throw new Win32Exception(Marshal.GetLastWin32Error());return status.EnergySaver==1;}
  public static void Awake(){
   IntPtr text=Marshal.StringToHGlobalUni("MineTeleop active operator console");
   try{Reason r=new Reason{Version=0,Flags=1,Data=new ReasonUnion{Text=text}};awake=PowerCreateRequest(ref r);}finally{Marshal.FreeHGlobal(text);}
   if(awake==IntPtr.Zero||awake==new IntPtr(-1)){awake=IntPtr.Zero;throw new Win32Exception(Marshal.GetLastWin32Error());}
   if(!PowerSetRequest(awake,1)||!PowerSetRequest(awake,0)){int e=Marshal.GetLastWin32Error();ReleaseAwake();throw new Win32Exception(e);}
  }
  public static void ReleaseAwake(){if(awake!=IntPtr.Zero){PowerClearRequest(awake,0);PowerClearRequest(awake,1);CloseHandle(awake);awake=IntPtr.Zero;}}
 }
}
'@
}

function Test-PowerAdministrator {
  return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-PowerCommandLine([string[]]$Values) {
  return (@($Values|ForEach-Object {'"'+([regex]::Replace([regex]::Replace($_,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}) -join ' ')
}

function New-PowerElevationCommand([string]$ScriptPath,[hashtable]$Parameters) {
  $request=@{}
  foreach($key in $Parameters.Keys){$v=$Parameters[$key];if($v -is [Management.Automation.SwitchParameter]){$v=[bool]$v};$request[$key]=$v}
  $data=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($request|ConvertTo-Json -Depth 4 -Compress)))
  $script=$ScriptPath.Replace("'","''")
  return "`$ErrorActionPreference='Stop';try{`$p=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$data'))|ConvertFrom-Json;`$a=@{};foreach(`$v in `$p.PSObject.Properties){`$a[`$v.Name]=`$v.Value};& '$script' @a;exit `$LASTEXITCODE}catch{[Console]::Error.WriteLine(`$_.ToString());exit 1}"
}

function Get-PowerStatePath {
  $directory=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'MineTeleop-Performance'
  if(Test-Path -LiteralPath $directory){
    if((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Power-state directory must not be a reparse point'}
    $owner=(Get-Acl -LiteralPath $directory).GetOwner([Security.Principal.SecurityIdentifier]).Value
    if($owner -notin @('S-1-5-32-544','S-1-5-18')){throw 'Existing power-state directory has an untrusted owner; refusing elevated recovery'}
  }
  else{New-Item -ItemType Directory -Path $directory|Out-Null}
  $acl=[Security.AccessControl.DirectorySecurity]::new()
  $admin=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
  $acl.SetOwner($admin);$acl.SetAccessRuleProtection($true,$false)
  foreach($sid in @($admin,[Security.Principal.SecurityIdentifier]::new('S-1-5-18'))){$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))}
  Set-Acl -LiteralPath $directory -AclObject $acl
  return Join-Path $directory 'active.json'
}

function Write-PowerState($State) {
  $file=Get-PowerStatePath
  $temporary=Join-Path (Split-Path $file) ([Guid]::NewGuid().ToString()+'.tmp')
  try {
    [IO.File]::WriteAllText($temporary,($State|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    $acl=Get-Acl -LiteralPath $temporary
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    Set-Acl -LiteralPath $temporary -AclObject $acl
    Move-Item -LiteralPath $temporary -Destination $file -Force
  } finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
}

function Enter-PowerLock {
  $file=Join-Path (Split-Path (Get-PowerStatePath)) 'session.lock'
  for($i=0;$i -lt 50;$i++){
    try{return [IO.File]::Open($file,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch [IO.IOException]{Start-Sleep -Milliseconds 100}
  }
  throw 'Another power-profile operation is still running'
}

function Get-PowerWifiAdapters {
  return @(Get-NetAdapter -Physical|Where-Object {$_.NdisPhysicalMedium -in @(1,9)})
}

function Wait-PowerWifiUp($Saved,[string]$Name) {
  # Some drivers finish Restart-NetAdapter before reassociating. Launching the
  # client then would miss its connection-scoped WLAN streaming requests.
  if(-not $Saved.PSObject.Properties['WasUp'] -or !$Saved.WasUp){return}
  $stable=0
  for($i=0;$i -lt 120;$i++){
    if([string](Get-NetAdapter -Name $Name).Status -eq 'Up'){$stable++}else{$stable=0}
    if($stable -ge 3){return}
    Start-Sleep -Milliseconds 500
  }
  throw 'Previously connected Wi-Fi did not reconnect within 60 seconds'
}

function Test-PowerProcess([int]$ProcessId,[string]$Started) {
  $p=Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  return $null -ne $p -and $p.StartTime.ToUniversalTime().Ticks.ToString() -eq $Started
}

function Get-PowerInspection {
  Initialize-PowerApi
  $plan=[MineTeleop.PowerProfile]::Active()
  $adapters=@(foreach($a in Get-PowerWifiAdapters){
    $p=Get-NetAdapterPowerManagement -Name $a.Name
    [pscustomobject]@{Guid=$a.InterfaceGuid;Name=$a.Name;AllowComputerToTurnOffDevice=[string]$p.AllowComputerToTurnOffDevice;SelectiveSuspend=[string]$p.SelectiveSuspend;DeviceSleepOnDisconnect=[string]$p.DeviceSleepOnDisconnect}
  })
  return [pscustomobject]@{Plan=$plan.ToString();WirelessAC=[MineTeleop.PowerProfile]::WifiIndex($plan,$true);WirelessDC=[MineTeleop.PowerProfile]::WifiIndex($plan,$false);EnergySaverAC=[MineTeleop.PowerProfile]::EnergyThreshold($plan,$true);EnergySaverDC=[MineTeleop.PowerProfile]::EnergyThreshold($plan,$false);EnergySaverActive=[MineTeleop.PowerProfile]::EnergySaverActive();Wifi=$adapters}
}

function Start-PowerProfile {
  Initialize-PowerApi
  $file=Get-PowerStatePath
  if(Test-Path -LiteralPath $file){throw 'A power-profile record already exists. Close its client, then run -Restore before starting another managed session.'}
  $self=Get-Process -Id $PID
  $devices=@(Get-CimInstance -Namespace root/wmi -ClassName MSPower_DeviceEnable -ErrorAction SilentlyContinue)
  $adapters=@(foreach($a in Get-PowerWifiAdapters){
    $p=Get-NetAdapterPowerManagement -Name $a.Name
    $device=@($devices|Where-Object {$_.InstanceName.StartsWith($a.PnPDeviceID+'_',[StringComparison]::OrdinalIgnoreCase)})
    if($device.Count -gt 1){throw 'Ambiguous Wi-Fi device power identity'}
    $low=@(Get-NetAdapterAdvancedProperty -Name $a.Name -RegistryKeyword LowPowerEnable -ErrorAction SilentlyContinue)
    $vendorLow=$a.PnPDeviceID -like 'PCI\VEN_14C3*' -and $low.Count -eq 1 -and @($low[0].ValidRegistryValues) -contains '0'
    [pscustomobject]@{Guid=$a.InterfaceGuid;Pnp=$a.PnPDeviceID;WasUp=([string]$a.Status -eq 'Up');DeviceInstance=if($device.Count){[string]$device[0].InstanceName}else{''};DeviceEnabled=if($device.Count){[bool]$device[0].Enable}else{$null};DeviceSupported=[bool]$device.Count;SelectiveSuspend=[string]$p.SelectiveSuspend;SleepDisconnect=[string]$p.DeviceSleepOnDisconnect;VendorLowPower=$vendorLow;LowPowerValue=if($vendorLow){@($low[0].RegistryValue)}else{@()}}
  })
  $state=[pscustomobject]@{Schema=1;Session=[Guid]::NewGuid().ToString();ParentId=$PID;ParentStarted=$self.StartTime.ToUniversalTime().Ticks.ToString();WatcherReady=$false;ChildId=0;ChildStarted='';OriginalPlan=[MineTeleop.PowerProfile]::Active().ToString();ManagedPlan='';Wifi=$adapters}
  Write-PowerState $state
  return $state
}

function Enable-PowerProfile($State) {
  # The GUID is journalled before any plan activation. OEM/Modern Standby
  # systems that reject the high-performance template fail without faking it.
  $State.ManagedPlan=[Guid]::NewGuid().ToString()
  Write-PowerState $State
  & (Join-Path $env:WINDIR 'System32\powercfg.exe') /duplicatescheme 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c $State.ManagedPlan|Out-Null
  if($LASTEXITCODE -ne 0){throw 'Windows does not permit a High Performance plan on this device; original settings will be restored'}
  & (Join-Path $env:WINDIR 'System32\powercfg.exe') /changename $State.ManagedPlan 'MineTeleop High Performance'|Out-Null
  if($LASTEXITCODE -ne 0){throw 'Cannot name the private performance plan'}
  [MineTeleop.PowerProfile]::WifiMaximum([Guid]$State.ManagedPlan)
  if($null -ne [MineTeleop.PowerProfile]::EnergyThreshold([Guid]$State.ManagedPlan,$false) -and $null -ne [MineTeleop.PowerProfile]::EnergyThreshold([Guid]$State.ManagedPlan,$true)){
    [MineTeleop.PowerProfile]::DisableAutomaticEnergySaver([Guid]$State.ManagedPlan)
  }else{Write-Warning 'Windows does not expose the automatic Energy Saver threshold on this device.'}
  [MineTeleop.PowerProfile]::Activate([Guid]$State.ManagedPlan)
  foreach($saved in $State.Wifi){
    $a=@(Get-PowerWifiAdapters|Where-Object {[Guid]$_.InterfaceGuid -eq [Guid]$saved.Guid -and $_.PnPDeviceID -eq $saved.Pnp})
    if($a.Count -ne 1){throw 'Wi-Fi adapter identity changed'}
    $restart=$false
    if($saved.DeviceSupported){$d=Get-CimInstance -Namespace root/wmi -ClassName MSPower_DeviceEnable|Where-Object InstanceName -eq $saved.DeviceInstance;if($d.Enable){$d|Set-CimInstance -Property @{Enable=$false}|Out-Null;$restart=$true}}
    foreach($key in @('SelectiveSuspend','DeviceSleepOnDisconnect')){
      $before=if($key -eq 'SelectiveSuspend'){$saved.SelectiveSuspend}else{$saved.SleepDisconnect}
      if($before -eq 'Enabled'){$parameters=@{Name=$a[0].Name;NoRestart=$true};$parameters[$key]='Disabled';Set-NetAdapterPowerManagement @parameters;$restart=$true}
    }
    if($saved.VendorLowPower -and $saved.LowPowerValue[0] -ne '0'){Set-NetAdapterAdvancedProperty -Name $a[0].Name -RegistryKeyword LowPowerEnable -RegistryValue '0' -NoRestart;$restart=$true}
    if($restart){Write-Host 'Applying Wi-Fi power settings; the adapter will briefly reconnect.';Restart-NetAdapter -Name $a[0].Name -Confirm:$false}
    Wait-PowerWifiUp $saved $a[0].Name
  }
  $actual=Get-PowerInspection
  if([Guid]$actual.Plan -ne [Guid]$State.ManagedPlan -or $actual.WirelessAC -ne 0 -or $actual.WirelessDC -ne 0){throw 'High-performance plan verification failed'}
  if(($null -ne $actual.EnergySaverAC -and $actual.EnergySaverAC -ne 0) -or ($null -ne $actual.EnergySaverDC -and $actual.EnergySaverDC -ne 0)){throw 'Automatic Energy Saver threshold verification failed'}
  if($actual.EnergySaverActive){throw 'Windows Energy Saver remains active (manual or managed policy). Turn it off in Windows Settings, then retry; saved power settings will be restored.'}
  foreach($w in $actual.Wifi){if($w.AllowComputerToTurnOffDevice -eq 'Enabled' -or $w.SelectiveSuspend -eq 'Enabled' -or $w.DeviceSleepOnDisconnect -eq 'Enabled'){throw 'Wi-Fi power-management verification failed'}}
  foreach($saved in $State.Wifi){if(-not $saved.DeviceSupported){Write-Warning 'The Wi-Fi driver does not expose device-auto-off control; this item could not be applied.'};if($saved.VendorLowPower){$a=Get-PowerWifiAdapters|Where-Object {[Guid]$_.InterfaceGuid -eq [Guid]$saved.Guid};if(@((Get-NetAdapterAdvancedProperty -Name $a.Name -RegistryKeyword LowPowerEnable).RegistryValue)[0] -ne '0'){throw 'Vendor idle-power setting verification failed'}}}
  [MineTeleop.PowerProfile]::Awake()
  return $actual
}

function Restore-PowerProfile([string]$Session='') {
  Initialize-PowerApi
  $file=Get-PowerStatePath
  if(!(Test-Path -LiteralPath $file)){return}
  $s=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json
  if($s.Schema -ne 1 -or ($Session -and $s.Session -ne $Session)){throw 'Power-profile identity mismatch'}
  $errors=@()
  foreach($saved in $s.Wifi){try{
    $a=@(Get-PowerWifiAdapters|Where-Object {[Guid]$_.InterfaceGuid -eq [Guid]$saved.Guid -and $_.PnPDeviceID -eq $saved.Pnp})
    if($a.Count -ne 1){throw 'Original Wi-Fi adapter unavailable; restoration remains pending'}
    $restart=$false
    if($saved.DeviceSupported -and $saved.DeviceEnabled){$d=Get-CimInstance -Namespace root/wmi -ClassName MSPower_DeviceEnable|Where-Object InstanceName -eq $saved.DeviceInstance;if($null -eq $d){throw 'Original device power control unavailable'};if(-not [bool]$d.Enable){$d|Set-CimInstance -Property @{Enable=$true}|Out-Null;$restart=$true}}
    foreach($key in @('SelectiveSuspend','DeviceSleepOnDisconnect')){$before=if($key -eq 'SelectiveSuspend'){$saved.SelectiveSuspend}else{$saved.SleepDisconnect};if($before -eq 'Enabled'){$p=Get-NetAdapterPowerManagement -Name $a[0].Name;if([string]$p.$key -ne $before){$parameters=@{Name=$a[0].Name;NoRestart=$true};$parameters[$key]=$before;Set-NetAdapterPowerManagement @parameters;$restart=$true}}}
    if($saved.VendorLowPower -and @($saved.LowPowerValue)[0] -ne '0'){$p=Get-NetAdapterAdvancedProperty -Name $a[0].Name -RegistryKeyword LowPowerEnable;if(@($p.RegistryValue)[0] -ne @($saved.LowPowerValue)[0]){if(@($p.RegistryValue)[0] -ne '0'){throw 'Idle-power option changed externally; refusing overwrite'};Set-NetAdapterAdvancedProperty -Name $a[0].Name -RegistryKeyword LowPowerEnable -RegistryValue @($saved.LowPowerValue) -NoRestart;$restart=$true}}
    if($restart){Restart-NetAdapter -Name $a[0].Name -Confirm:$false}
    Wait-PowerWifiUp $saved $a[0].Name
    $p=Get-NetAdapterPowerManagement -Name $a[0].Name
    if($saved.DeviceSupported -and $saved.DeviceEnabled -and [string]$p.AllowComputerToTurnOffDevice -ne 'Enabled'){throw 'Device-auto-off restore verification failed'}
    if($saved.SelectiveSuspend -eq 'Enabled' -and [string]$p.SelectiveSuspend -ne 'Enabled'){throw 'Selective-suspend restore verification failed'}
    if($saved.SleepDisconnect -eq 'Enabled' -and [string]$p.DeviceSleepOnDisconnect -ne 'Enabled'){throw 'Device-sleep restore verification failed'}
    if($saved.VendorLowPower -and @($saved.LowPowerValue)[0] -ne '0' -and @((Get-NetAdapterAdvancedProperty -Name $a[0].Name -RegistryKeyword LowPowerEnable).RegistryValue)[0] -ne @($saved.LowPowerValue)[0]){throw 'Idle-power restore verification failed'}
  }catch{$errors+=$_.Exception.Message}}
  try{if($s.ManagedPlan){$managed=[Guid]$s.ManagedPlan;if([MineTeleop.PowerProfile]::Active() -eq $managed){[MineTeleop.PowerProfile]::Activate([Guid]$s.OriginalPlan)};[MineTeleop.PowerProfile]::Delete($managed)}}catch{$errors+=$_.Exception.Message}
  [MineTeleop.PowerProfile]::ReleaseAwake()
  if($errors.Count){throw ('Power restoration incomplete; saved state retained: '+($errors -join '; '))}
  Move-Item -LiteralPath $file -Destination (Join-Path (Split-Path $file) 'last-restored.json') -Force
}

Export-ModuleMember -Function Initialize-PowerApi,Test-PowerAdministrator,ConvertTo-PowerCommandLine,New-PowerElevationCommand,Get-PowerStatePath,Write-PowerState,Enter-PowerLock,Test-PowerProcess,Get-PowerInspection,Start-PowerProfile,Enable-PowerProfile,Restore-PowerProfile
