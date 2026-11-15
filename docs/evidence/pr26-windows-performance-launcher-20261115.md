# Windows managed performance startup, 2026-11-15

The optional administrator launcher was tested on the existing Windows MT7921
host, original driver **3.0.1.1336**. No vehicle session or input was used in
these launcher lifecycle checks. VPN, roaming, router and control safety
thresholds were not changed. These results do not establish improved latency
or sustained control acceptance.

## Hardware settings and recovery

| Setting | Before | Managed client running | After recovery |
| --- | --- | --- | --- |
| Active power plan | Original OEM balanced | Private High Performance copy | Original OEM balanced |
| Wireless AC / battery policy index | 0 / 2 | 0 / 0, maximum performance | 0 / 2 |
| Automatic Energy Saver AC / battery threshold | 0% / 30% | 0% / 0% | 0% / 30% |
| Windows reported Energy Saver active | false | false | false |
| Allow computer to turn off WLAN device | Enabled | Disabled | Enabled |
| MediaTek `LowPowerEnable` | 1, Auto | 0, Disabled | 1, Auto |
| `RoamIndicateTh` | 4 | 4 | 4 |
| NDIS selective suspend / sleep on disconnect | Unsupported | Unsupported | Unsupported |

This device exposes `MSPower_DeviceEnable` for its exact WLAN PnP identity.
That changes the Windows device-auto-off checkbox, distinct from the driver's
idle-power option tested in earlier trials. Settings are read back after
application and restoration. Original power plans are not edited. The private
copy is deleted after restoration; a retained recovery journal blocks a new
managed session until recovery succeeds.

## Executed checks

The final sequential validation task completed with exit 0 and `PASS`:

- Actual controller `--help`: exit 0, original settings restored.
- Unknown controller argument: exit 2, original settings restored.
- Existing client / manual restore during an active managed session: both
  rejected with exit 1, first session and client preserved.
- Force termination of the launcher: independent watcher stopped the recorded
  client, restored the settings and removed the active record.
- Invalid executable: process creation failed, reported exit 1, original
  settings restored and no active record remained.
- Native argument sink and generated elevation payload: empty strings, spaces,
  quotes, trailing backslashes, Chinese text and shell-looking text survived
  unchanged. Default/no-argument invocation, exit 7 propagation and a terminating
  startup exception returning 1 also passed. Read-only inspection did not alter
  settings.

Controller executable SHA-256:
`d28f4fd1efc4cd664c1c6cdf28b3a85e3070a7c2e8f31152aa0cff99bee4248f`.
It is the existing `106f4b1` product build; this change does not modify C++ or
web control code. Git comparison of its product source to the parent of this
change was empty for `cpp/src`, `cpp/include` and `cpp/web`.

The first lifecycle harness did not retain a Windows process handle before
waiting, leaving its exit-code observation empty; the failed receipt is kept.
A deliberately invalid-executable fixture initially stalled with sequential
stdout/stderr reads; it was explicitly terminated and restored. Concurrent
draining fixed that fixture. The next attempt exposed a real elevation-wrapper
bug: a terminating script error could be followed by `exit $LASTEXITCODE` using
the earlier successful `powercfg` status, falsely returning 0. The wrapper now
has an explicit terminating-error catch returning 1, covered by the final
regression and invalid-executable test. Watcher output is also isolated into
protected files, and unnecessary progress output is suppressed. Failed
receipts were retained, not counted as passes.

An initial live-session preparation exposed another startup race: after the
WLAN reset the controller reported zero connected/enabled streaming interfaces.
The launcher now waits for the originally connected WLAN to return before
starting the client. Lifecycle validation was rerun successfully. The next
actual controller startup reported one connected interface, one streaming
request, one disabled-background-scan request and error zero. That observation
does not by itself prove latency improvement. The initial preparation also
ended with a Windows-recorded controller access violation (`0xc0000005`), whose
root cause is not established. It must not be described as fixed by the wait.

## Remaining limits

An interactive UAC consent click was not exercised remotely: hardware mutation
tests used an explicitly authorized, elevated interactive test task. Parameter
serialization for that UAC path was executed without elevation. Native viewer
and Electron package entry points share the launcher, but their complete live
UI sessions were not exercised by these lifecycle checks. Modern Standby,
unsupported OEM power templates, unplugged-battery operation, forced Energy
Saver / enterprise policy, reboot recovery and physical driver failure remain
unverified. Such cases must not be described as guaranteed recovery.

The user requested a new continuous 60-minute Windows browser / Orin
eight-direction-plus-brake mock run under this profile. Its control acceptance
and network measurements are recorded separately; startup checks alone cannot
make the previous failed Windows soaks pass.
