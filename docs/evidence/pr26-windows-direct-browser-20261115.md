# Windows power / direct browser diagnostics, 2026-11-15

**Continuous 60-minute control acceptance FAILED.** The power launcher passed
its separate lifecycle checks. None of the control prefixes below constitutes
a 60-minute pass; prefixes were not joined and control was not automatically
rearmed after a failed condition.

## Scope and unchanged product

These are the explicitly authorized local routed Windows Edge → Mac Ethernet /
Internet Sharing → Orin tests, not independent WAN or 5G. Mac `en0` is Ethernet;
`en1` is Wi-Fi. The Windows test route used the Mac Ethernet address. The second
UDP echo endpoint traverses a different Mac interface and is not interchangeable
with that route.

Two real ISX031 streams, the scene/model UI and ordinary keyboard handlers
remain enabled. Inputs cycle through forward, reverse, left, right, four
diagonals and Space brake. Mock only: CAN down, zero speed, zero motor torque,
five-degree steering, clock uncertainty 25 ms, command gap 200 ms, watchdog
800 ms, critical video 500 ms, consecutive late limit three, transient hold off.
VPN and router settings were not modified.

Launcher source: `8b5144d297bcbd0d59aad9043d0cd52b8f614138`. Controller product
source remains `106f4b1`; controller SHA-256:
`d28f4fd1efc4cd664c1c6cdf28b3a85e3070a7c2e8f31152aa0cff99bee4248f`.
Orin executable SHA-256:
`544699c324f5e6b4c49df5e420d7c94a0d4968c57954592ee62099d80c889620`.
No C++ control or safety changes were made for these experiments. The original
WLAN driver is 3.0.1.1336. The optional power launcher does not change roaming
or the browser's proxy configuration by default.

## Distinct attempts

Spans include samples in the failed cycle, through first detection. They are
not a guarantee of uninterrupted direct operation up to detection: native logs
can reveal a transition between UI samples. A wrapper task returning zero
means cleanup completed; it does not override a failed test exit.

| Run | Change / observation | Span through detection | Complete nine-input cycles | Verdict |
| --- | --- | ---: | ---: | --- |
| R4 | Attempt direct non-loopback HTTP/WS | 0 s | 0 | Product correctly rejected non-TLS public signaling; no scored input |
| R5 | Direct TLS signaling, original roaming 4 | 1597.574 s | 64 | Control-link protection; native delivery missing and independent UDP receive gaps |
| R6 | Direct TLS + temporarily roaming 0 | 86.340 s | 3 | Vehicle DEGRADED; result/telemetry snapshot was stale |
| R7 | Original roaming; local HTTP timing observer | 85.459 s | 3 | Local requests blocked in proxy resolution; bridge timed out |
| R8 | Isolated Edge process `--no-proxy-server` | 1630.652 s | 65 | 295 ms main-thread long task; input lease expired and WSS selected |
| R9 | Same direct browser + main-thread profiler | 1104.928 s | 44 | No corresponding long task; independent network receive gap, vehicle DEGRADED |
| R10 | Direct browser + temporary roaming 0 + profiler | 1671.134 s | 67 | Native WSS interval, then DC requalification; direct-continuity acceptance failed |
| R11 | Same combination + Orin kernel timestamps | 202.614 s | 8 | Invalidated by test guard focus interference; not a transport/safety failure |
| R12 | Retired completed connectivity guard before input; kernel timestamps retained | 181.579 s | 7 | Real control-link degradation with a kernel RX gap |

Earlier failed prefixes and the first archive remain in
[the preceding record](pr26-windows-performance-control-20261115.md).

## Three different failure mechanisms

### Local browser proxy-resolution wait

R7's DevTools metadata records approximately 1972 ms in `proxyStart` →
`proxyEnd` for a request to `127.0.0.1`. At the same incident, bridge requests
aborted after about 500 ms before a normal response. The successful local
request handlers around the fault did not show matching processing delays.
This is evidence of a browser networking stall before request dispatch, not
vehicle WebRTC callback queueing.

In R8, 148,295 responses recorded **zero proxy-resolution samples** with a
separate Edge profile and `--no-proxy-server`. System proxy, VPN and other
browser profiles were unchanged. This removed that observed stall, but did
not produce a 60-minute pass. Microsoft documents these
[process command-line options](https://learn.microsoft.com/en-us/deployedge/edge-learnmore-cmdline-options-proxy-settings);
managed [ProxySettings policy](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/proxysettings)
can override them. This is an experiment/workaround, not a new default in the
power launcher.

### Main-thread scheduling stall

R8's failure interval records a 295 ms long task and 294.5 ms timer lag. The
independent UDP probes were around 32 ms maximum in that interval and proxy
resolution was absent. The native sender kept running; the browser courier
stopped delivering new commands during the long task. This exceeds the
unchanged 200 ms input lease.

The exact function causing R8's task was not captured. R9/R10 CPU profiles did
not reproduce that task at their failures; they must not be presented as proof
that rendering or garbage collection caused R8. Heap and DOM counters did not
show sustained growth in R9. No speculative scene rewrite or safety relaxation
was applied.

### RX gap before the vehicle application callback

R12 finally reproduced a real degradation with the timestamp capture active.
Local HTTP remained short, main-thread long-task count was zero and timer lag
was about 10 ms. Native sender wakeup lag was at most 2 ms in the measurement
window; the vehicle control-mutex wait was at most 1 ms.

The incoming DTLS kernel record stream had a **491.022 ms gap** ending at Orin
monotonic time `625690237.037 ms`. The next application callbacks entered at
`625690237/239 ms`. The vehicle then reported control-link degradation with its
last valid command 524 ms old. Sequences 3671–3677 have no correlated vehicle
result in the measured dispatch cohort. The capture had **zero socket drops**
and zero missing kernel timestamps; the maximum packet-reader delay near the
incident was 1.339 ms. Windows-to-Mac ordinary UDP also had a 581.371 ms receive
interval in the corresponding incident window.

This supports a delivery interruption before the userspace vehicle callback,
rather than a long GStreamer callback/control-mutex queue in this incident.
It does **not** identify a unique Windows NIC, AP, shared-routing component or
kernel/driver cause. `SO_TIMESTAMPNS` is a software receive timestamp, not a
physical NIC hardware timestamp; pre-hook driver/softirq delays remain possible.
The hook observed incoming packets only. DTLS header sequence numbers cannot
identify individual encrypted SCTP commands, so temporal correlation is not
claimed as a per-message decrypted mapping.

The capture was fixed to the two authorized mock endpoints, bounded to 3600 s,
and recorded only timestamps, ports, length, record type/epoch/sequence. It did
not save payloads. Parser checks covered valid records, wrong peer, truncation,
fragmentation and non-UDP rejection. An initial CLI IPv4 conversion error was
corrected before R11 capture; that failed preparation was not scored as capture
evidence. R12 captured the scored failure, not a synthetic replacement.

| R12 measured dispatch cohort | Result |
| --- | ---: |
| Correlated accepted results | 3519 |
| Accepted-result RTT p50/p95/p99/max | 19/31/53/502 ms |
| Native dispatches | 3527 DC / 1 WSS |
| Missing correlated results | 7 |
| Correlated rejected results | 2 (`degraded_neutral_required`) |
| Vehicle protection events | 1 |
| Sampled video / clock | Both fresh; uncertainty 9–11 ms |

RTT is native dispatch to correlated software result receipt, not physical
actuation. Accepted-result percentiles cannot conceal missing/rejected tails.
Native command timestamps use the signaling time domain; the analysis maps
them through accepted clock anchors before selecting the Windows measurement
window. Vehicle/kernel comparison uses the Orin monotonic clock. Receipts for
in-window dispatches may arrive after the last UI sample. The browser continued
to display `dc` in sampled status while the native trace already contained a
WSS dispatch; a zero stop-sequence field also does not prove no braking occurred.

## Test interference and recovery

R11's `window_blur` at 19:01:08.502 UTC coincided with the already-completed
five-minute connectivity guard's PowerShell startup (engine events at
19:01:08.708–08.715 UTC). All 3937 measured commands were accepted over DC, with
no in-window vehicle protection and no kernel RX gap. Its failure was cleared
input after focus loss, not a proven network failure. R12 removed that completed
guard immediately after SSH verification, while retaining the 90-minute restore
and normal cleanup. No focus or input safety check was removed.

Final readback at 19:21:02 UTC verified original plan
`85d583c5-cf2e-4197-80fd-3789a227a72c`, wireless AC/DC 0/2, Energy Saver 0/30,
device-auto-off Enabled, idle power 1, roaming 4, driver 3.0.1.1336. No active
managed journal/private plan/sleep request/test route or temporary per-app WER
key remained. Thirteen additional owned completed tasks were removed; no owned
client/observer remained. All Orin runners/captures stopped, cameras were
released, CAN stayed down, and 27 temporary remote authentication/private-key
files were removed. Mac echo listeners stopped. Sharing remained as authorized;
VPN was not modified.

## Acceptance boundary

The administrator launcher and rollback behavior are verified separately in
[its hardware record](pr26-windows-performance-launcher-20261115.md).
Power-saving changes, a direct browser proxy mode and roaming-off did not make
this test path meet the requested continuous 60-minute criterion. A new run
needs a concrete correction or changed link condition; another lucky prefix
would not establish the cause or remove the known main-thread and delivery-gap
risks. No WAN/5G, real vehicle, or native-versus-browser superiority is claimed.

Raw diagnostic files, parser/analysis scripts and recovery receipts are archived
separately from the immutable first-three-run archive. Credentials, browser
profiles, CLI history, executables, crash dumps and private TLS keys are excluded.
Verified archive: `/Volumes/SystemDisk/Workspace/mine-teleop-artifacts/pr26-windows-power-diagnostics-20261115/`.
The manifest covers 446 files / 1,634,820,603 bytes; `checksums.json` SHA-256:
`0d4589d403685844dbb9746d81923f864d4f397f216582e28c9c3c3a47b11f31`.
All 99 Orin and 129 Windows raw files matched their source byte checksums;
the additional harness, analysis and receipts are included in the final manifest.
Exact temporary-authentication scans and sensitive-field scans found no matches.
The preceding archive was not overwritten.

The tested launcher source `8b5144d` passed [offline regressions](https://github.com/JR-Rong/MineTeleop/actions/runs/43733523736), [Windows native receiver CI](https://github.com/JR-Rong/MineTeleop/actions/runs/43733523743), and [build/release CI](https://github.com/JR-Rong/MineTeleop/actions/runs/43733523882). Those checks do not override the failed hardware control acceptance.
