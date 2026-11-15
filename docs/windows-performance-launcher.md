# Windows administrator performance launcher

Windows control bundles, Electron desktop bundles and native viewer artifacts
include `Start-High-Performance.cmd`. Double-click it and approve UAC. It detects
the client beside it and starts that client after applying and verifying the
performance settings. Running an executable directly keeps its previous behavior.
This feature does not change control transport, safety gates or vehicle code.

The launcher saves the active power plan and the supported physical WLAN power
options before changing anything. It creates a private copy of Windows High
Performance, selects maximum wireless performance for AC and battery, sets the
automatic Energy Saver battery threshold to 0%, disables
the device-auto-off checkbox through its exact PnP device identity, and disables
supported selective suspend / sleep-on-disconnect options. On supported MediaTek
PCI WLAN devices it also disables `LowPowerEnable` idle saving. It does not
blanket-disable offloads, wake settings, roaming, virtual adapters or VPNs.
Unsupported settings are reported rather than claimed as applied. Driver
changes may briefly reconnect Wi-Fi, before the client starts.
An originally connected WLAN must return to a stable `Up` state within 60
seconds before client startup, so startup-scoped WLAN requests are not missed
during reassociation. Restoration also waits for that adapter to return.

While running, a process-scoped Windows power request keeps the system and
display awake. This does not prevent manual sleep, emergency battery shutdown,
hardware/driver failure or network congestion. It checks Windows' actual Energy
Saver flag before starting the client; if a manual or managed policy keeps it
on, startup fails and restores settings, asking the operator to turn it off in
Settings. It does not force undocumented global policy changes. High Performance can increase battery
drain, heat and fan noise. OEM and Modern Standby machines may reject that
template; startup then fails and restores the saved settings.

## Usage and recovery

Use 64-bit Windows PowerShell 5.1. The CMD entry prompts for UAC automatically;
PowerShell also offers explicit targets and application arguments:

```powershell
.\run-high-performance.ps1 -Target control
.\run-high-performance.ps1 -Target desktop
.\run-high-performance.ps1 -Target native -GStreamerRoot 'C:\gstreamer\1.0\msvc_x86_64'
.\run-high-performance.ps1 -Target control -ClientArguments @('--port','8080')
.\run-high-performance.ps1 -Inspect  # read-only, no UAC or settings changes
.\run-high-performance.ps1 -Restore # UAC, recover an inactive saved session
```

Keep the launcher window open during use. Closing the client normally restores
settings. If the launcher is terminated, an independent elevated watcher stops
its recorded client PID and restores settings; PID creation times protect
against PID reuse. A browser tab is not the local control server: closing the
tab alone leaves the server and its managed power session running. Close the
launcher when finished. There is no persistent service or production scheduled
task. Only one managed client may run at a time.

Snapshots are restricted to Administrators and SYSTEM in
`%ProgramData%\MineTeleop-Performance\active.json`. They contain local device
identifiers and power options, not credentials. Successful restoration moves
the record to `last-restored.json`; incomplete restoration retains it and reports
an error for manual `-Restore`. Reboot, power loss, an unavailable adapter or
termination of both launcher and watcher can require this manual recovery. Do
not delete the record to bypass recovery. An active launcher cannot be restored
by a second instance. An externally selected different power plan is preserved;
the launcher's private plan is removed. Original plans are never edited.
The watcher writes `watcher.out.log` / `watcher.err.log` in the same protected
directory, independently of the launcher's console or redirected output.
Client exit status is reported as `windows_performance_client_exited` and
propagated; terminating startup errors report failure rather than reusing an
earlier successful utility exit status.

These changes remove local power-saving variables. They do not establish a
cause for previous Wi-Fi stalls, promise WAN/5G direct connectivity, or establish
60-minute control acceptance. Keep those results separate from launcher checks.

The power-plan settings use Microsoft's documented
[Energy Saver threshold](https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/battery-threshold)
and [subgroup](https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/energy-saver-settings).
The active state comes from
[SYSTEM_POWER_STATUS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status).

## Validation

CI parses the scripts and runs the read-only argument/inspection tests in
`scripts/test/windows_performance_launcher_tests.ps1`. They use a real native
argument sink to check spaces, empty arguments, quotes, trailing slashes and
shell-looking text, and execute the generated UAC payload without elevating to
check parameter preservation and exit status. Hardware validation must also
check before/during/after power settings, normal exit, launcher termination and
failed startup. A VM or argument test cannot prove WLAN device behavior.

The [Windows hardware validation record](evidence/pr26-windows-performance-launcher-20261115.md)
records the actual adapter settings and recovery checks. It is separate from
the 60-minute directional control test.
