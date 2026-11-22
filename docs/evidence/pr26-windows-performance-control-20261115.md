# Windows High Performance control attempts, 2026-11-15

**Acceptance FAILED. No continuous 60-minute control run completed.** Prefixes
were not combined, restarted timers were not treated as continuous operation,
and protection braking was not hidden by an unchanged stop-sequence field.
The managed power launcher works, but this test does not show that it solves
the active-session control stalls.

## Exact scope

These are Windows Edge to Orin tests over the explicitly authorized local
routed network through Mac Internet Sharing, not independent WAN/5G. Two real
ISX031 cameras use the existing CPU H.264 profile. The existing scene/model UI
and keyboard handlers remain enabled. WASD cycles through forward, reverse,
left, right, four diagonals and Space brake. Completed cycles require
correlated vehicle acceptance, matching D/R gear, nonzero mock steering for
left/right and mock brake feedback. They do not prove physical execution.

All runs use mock, CAN down, zero speed, zero motor torque and a confirmed
five-degree steering profile. The clock limit remains 25 ms, command gap
200 ms, watchdog 800 ms, critical-video limit 500 ms and consecutive-late limit
three. Transient hold is disabled. VPN, router and roaming are unchanged;
the original WLAN driver is 3.0.1.1336. The launcher selected a private High
Performance plan, maximum wireless AC/DC performance, automatic Energy Saver
threshold 0%, device-auto-off Disabled and supported MediaTek idle power 0.

Launcher/runtime source was `990533c`. The executable is the existing
`106f4b1` controller, SHA-256
`d28f4fd1efc4cd664c1c6cdf28b3a85e3070a7c2e8f31152aa0cff99bee4248f`;
its product source was unchanged. Orin SHA-256 is
`544699c324f5e6b4c49df5e420d7c94a0d4968c57954592ee62099d80c889620`.
The later downloaded-script compatibility change affects launcher execution
policy only and was checked separately without changing power settings; it is
not a new 60-minute control result.

## Measured results

| Attempt | Valid span | Full nine-input cycles | Failure |
| --- | ---: | ---: | --- |
| Initial preparation | 0 s of scored input | 0 | Profile invalid / WSS fallback / TIMEOUT_BRAKE before first input; later controller access violation |
| After waiting for WLAN reconnection; Mac-hosted CDP | 55.273 s | 2 | Cycle 3 right: vehicle telemetry 1030.5 ms old; vehicle already DEGRADED / braking wait |
| Same client/profile; Windows-local CDP and keyboard script | 36.094 s | 1 | Cycle 2 forward-right: telemetry 1244.7 ms old; vehicle already DEGRADED / braking wait |

The earlier progress report of approximately 42 seconds for attempt 2 was the
last completed-cycle progress. It omitted samples in the failed cycle. The
final span above includes every sample through failure. Attempt 3's last
completed-cycle progress was similarly shorter than its true 36.094-second
failure span. Analysis computes span from all samples, including the failed
record.

The last browser snapshots still said CONTROL_ACTIVE and `dc`, with fresh
video, 10/11 ms clock uncertainty, focus retained and host/host IPv4 UDP media.
Those control/status fields were stale: native dispatch traces show **23/28 WSS
dispatches** within the respective measurement windows. Vehicle logs record
control-link protection with the last accepted commands **253/240 ms** old.
The zero stop sequence does not mean there was no protection braking.

| Short failed prefix only | Mac-hosted CDP | Windows-local CDP |
| --- | ---: | ---: |
| Correlated accepted results | 1050 | 673 |
| Accepted-result RTT p50/p95/p99/max | 18/32/42/83 ms | 19/32/51/76 ms |
| Sender wakeup-lag maximum | 3 ms | 2 ms |
| Native send-call maximum | 1 ms | 1 ms |
| Vehicle control-mutex wait maximum | 0 ms | 0 ms |

RTT is native dispatch to correlated result receipt, not physical actuation
or video latency. Rejected results and missing delivery are kept separately.
The analysis selects command sequences by their send time inside the window,
including those newer than the last accepted result; it does not truncate the
failed tail at that accepted sequence. Cross-host event alignment uses the
accepted clock anchors and remains subject to both endpoints' uncertainty.

The previous Mac browser local-link run reported 1846.570 seconds with RTT
16/28/31/59 ms, as recorded in
[the earlier investigation](pr26-windows-wlan-streaming-20261114.md).
These short Windows failures, different network paths and different test
automation do not establish an OS or native-versus-browser advantage.

## What the evidence narrows down

- The initial controller started before WLAN reassociation and reported zero
  enabled streaming interfaces. The added reconnect wait changed the next
  real startup to one connected interface, one streaming request, one disabled
  background-scan request and error zero. This fixes that startup race.
- The initial executable later crashed with Windows event 1000 and
  `0xc0000005`. There is no usable first-crash dump. Its root cause remains
  unknown; it must not be described as fixed by the reconnect wait.
- Active UI requests to the local control service timed out around the
  failures, including the transport bridge. In the successful requests that
  were recorded, intent processing, mutex waits and bridge processing were
  at most 1 ms. That excludes a long wait in those recorded handlers, not
  browser connection queueing, socket delivery, unrecorded requests or response
  handling.
- Moving the same pinned Playwright CLI 0.1.23 workload to Windows did not
  prevent the failure. Cross-Wi-Fi CDP is therefore not a sufficient explanation.
  Node 22.20.0 was portable in the test directory, checked against the official
  ZIP SHA-256; no system installation or persistent PATH change was made.
- A separate no-login/no-vehicle loopback health probe completed 1200 requests:
  p50 0.1605 ms, p99 0.5318 ms, max 7.6248 ms. Closing at requests 512 and 1024
  did not reproduce the delay. This does not test active mixed UI traffic.
- Ordinary UDP to one Mac endpoint stayed below 78/63 ms during the two
  prefixes; the other endpoint reached 567/191 ms. Endpoint paths differ.
  These observations do not identify a NIC, AP, OS component or browser cause.

Further work should instrument active-session HTTP queueing, response delivery
and browser bridge processing, and capture a reproducible access violation.
Changing safety thresholds would not resolve those missing observations.

## Cleanup and verification

Windows original power plan, wireless 0/2 indices, Energy Saver 0/30 thresholds,
device-auto-off Enabled, idle power 1, roaming 4 and driver 3.0.1.1336 were
read back after all attempts. No active managed journal, private power plan,
sleep request or test route remained. Eleven owned test tasks were removed.
The temporary application-specific WER capture settings were restored; no
global WER policy was changed. Task-wrapper exit 0 records cleanup, not control
acceptance: the local test exit was 1 and the failed samples are authoritative.
All Orin runners reported stopped, cameras were released and CAN remained down.
Temporary authentication inputs were removed. Mac sharing was left as authorized;
owned SSH forwards and UDP probes were closed. VPN was not modified.

The launcher's independent lifecycle checks and downloaded-script checks are in
[the startup validation record](pr26-windows-performance-launcher-20261115.md).
`990533c` passed offline regressions and the Windows native receiver CI job,
including the launcher checks and native compilation. CI success does not
override these failed hardware control attempts.

Original evidence and the reproducible analysis are archived at
`/Volumes/SystemDisk/Workspace/mine-teleop-artifacts/pr26-windows-performance-20261115/`.
The manifest covers 134 files / 121,120,464 bytes; `checksums.json` SHA-256 is
`a89923553efe6d8af8a2e400e9b455da5622d9447b65c5908f331f0ab6444f09`.
All listed hashes were verified after copying. Authentication files, browser
profiles, CLI history and crash dumps are excluded. Earlier failed fixtures and
all three failed prefixes remain distinct. ANSI error logs retain their original
bytes; they were not converted to UTF-8.
