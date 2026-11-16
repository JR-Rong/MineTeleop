# Windows send-path investigation, 2026-11-15

This is a mock-only, Mac-shared routed-network investigation. It is not WAN,
5G, CAN actuator, or a passed 60-minute control acceptance result. PR26 still
bases on PR25. No VPN or safety thresholds were changed.

## What the failed runs distinguish

| Trial | Windows WLAN setting | Continuous scored seconds | Completed eight-direction/brake cycles | Result |
| --- | --- | ---: | ---: | --- |
| A1 | Original 1336 driver, ac, roaming 0, performance launcher | 528.087 | 21 | Failed |
| A2 | Temporary 1338 driver, ac, roaming 0 | 649.599 | 26 | Failed |
| A4 | Original 1336 driver, ax, roaming 0 | 739.656 | 29 | Failed |
| A5 | Original 1336 driver, ax, kernel timestamps | 627.402 | 25 | Failed |
| A6 | Original 1336 driver, ax, complete miniport metadata and gateway probe | 433.515 | 17 | Failed |
| A8 | Worker courier, 1336 ax, roaming 0 | 1146.259 | 46 | Failed, physical wireless-path loss |
| A9 | Worker courier, 1336 actual ac/VHT, roaming 0 | 192.352 | 7 | Failed, physical wireless-path loss |
| A10 | Worker courier, 1336 ac; continuous WLAN queries removed | 534.242 | 21 | Failed, time-probe TCP connection timeout and fresh-neutral interlock |

A3 was preparation only. The temporary 1338 driver was restored to 1336;
performance/radio/roaming settings were restored between trials. A newer 1441
driver was only downloaded and signature/hardware checked; installation has not
been authorized or performed.

A5 demonstrates a real packet-delivery gap. The primary UDP probe sent 6,376
packets and the Mac received 6,368. Receive gaps reached 490.182 and 606.165 ms;
completed probe round trips still peaked at only 79.9 ms. TCP/IP send events
continued through the eight missing sequence slots. In the complete retained
per-CPU fault interval, the largest observed DPC was 0.2394 ms, with no reported
ETW loss. The localization is after the TCP/IP send hook. These observations
do not uniquely identify the Wi-Fi driver, firmware, access point, or receiving
network as the cause. Inbound vehicle datagrams also had long gaps.

A6 demonstrates a separate page scheduling failure. All 4,881 primary probe
packets arrived, and all 2,055 scored physical-gateway probes succeeded (maximum
native round trip 65 ms). Around the failure the page recorded twelve long tasks
over two seconds, a 113 ms maximum task, 126 ms timer lag, and 247 ms local input
HTTP elapsed time. The original page courier discarded already-expired frames
8578 and 8584. Native input remained fresh immediately before the transport
boundary. Nine commands selected WSS; this trial failed even though sampled
stop counters remained zero. Native protection and input-clear records are
part of acceptance, not just periodic dashboard snapshots.

## Capture coverage correction

A 64 MB PacketMon ring retained only a subset of CPU buffers, despite reporting
no lost events. Missing exported rows from those early captures cannot prove
that a packet missed the miniport. A separate 512 MB bounded-ring calibration
exported all 275 sends for each of two fixed probe flows across fifteen active
CPUs. A6 used the calibrated method with a 1,024 MB upper bound and compressed,
metadata-only, three-flow filtering. The physical miniport identity was unchanged
at start and stop. Zero MAC fields in native Wi-Fi metadata are not ARP evidence.
Kernel CSV circular retention must likewise be trimmed to the intersection of
retained CPU intervals before computing gaps.

## Targeted repair: independent courier, unchanged input authority

`control_transport_worker.js` transfers the actual unordered, zero-retransmit
control RTCDataChannel synchronously in `ondatachannel`. It runs the existing
ordinary and emergency courier factories in a dedicated Worker. No second
signer, input sampler, wire timestamp, automatic rearm, or duplicate command
carrier was added. A bounded trace mailbox keeps a delayed page from creating
an unbounded diagnostic queue. Unsupported channel transfer falls back to the
existing page courier. `?control_courier=page` selects that comparison path.

The page publishes its current session/UI/media scope with a monotonic timestamp;
a Worker rejects scope publications at 200 ms. The native bridge independently
revalidates current input ownership, epoch, intent and safety revisions and caps
each wire frame's publication allowance to its original remaining input lease.
A newer UI refresh cannot extend an older frame. Vehicle command, video, ACK,
clock, and watchdog gates remain authoritative. Worker failure closes the facade;
it does not create input authority or replay cached motion.

Windows Edge 157 local paired-RTCDataChannel testing exercised the actual module,
not a replacement courier: a 120 ms page pause allowed five unchanged frames to
be forwarded by the Worker; during a 350 ms pause, its last send was 140 ms after
pause start and it then stopped. This is a capability/deadline test, not vehicle
acceptance. Automated tests additionally cover late HTTP, expired/retired scope,
native publication expiry, actual channel pressure, and bounded diagnostics.

The A5 physical-path burst remains unresolved by this software change. A full
strict 60-minute, two-video, eight-direction-plus-Space mock run must still pass
on the built repair before acceptance can be claimed.

## Follow-up hardware-path and control evidence

A7 passed three complete two-camera mock direction/brake cycles and explicit
page-pause injections. With the actual Worker courier, commands continued during
a 120 ms page pause. A 350 ms pause stopped fresh input at the existing 200 ms
lease and required neutral input; held motion was not revived. This short run is
not a 60-minute acceptance.

A8 independently sent 11,131 primary UDP probes; the Mac received 11,125. All
six absent sequences reached the physical MT7921 miniport Tx hook within
1.449–2.758 ms of their send-call starts. Gateway probes timed out concurrently.
Vehicle receive gaps at the same miniport were 417.753 and 487.214 ms. DPC
coverage spans the fault on all sixteen CPUs; maximum DPC duration was 0.3068 ms.
There was no page long task, and the Worker continued its sends during the
ACK gap. The fault is beyond the recorded miniport Tx entry, with a common
wireless path affected in both directions. A Tx hook is **not** radio transmission
or send-completion proof: driver, firmware, RF and AP are not yet separated.

A9 reproduced loss using actual ac/VHT (PHY type 8), not merely a configured
registry value: eight probes were absent, receive gap 618.699 ms, and gateway
probes timed out. Merely selecting Wi-Fi 5 and high performance did not fix it.

A10 sent/received all 5,577 primary probes (maximum receive gap 140.094 ms).
There were no UI long tasks. However, time candidate 19 completed two good
four-timestamp exchanges, then its third TCP connection timed out after
501.232 ms before any HTTP request was sent. The native time-refresh catch
invalidated held input; `fresh_neutral_required` cleared W+D. The native clock
trace records the failure before that clear and a healthy replacement cohort
afterward. This evidence does not support changing input-store sampling or
waiving the 25 ms clock requirement. The passive WLAN whitelist trace recorded
zero scan/RPC events; an independent known-scan calibration proved the event
filters and caller-PID projection. It does not identify scanning as the cause.

Native `send_completed` for DC is a bridge publication, not a wire send. Use
Worker `dc_command_sent` records when counting actual transmissions; legitimately
superseded publications are not packet-loss evidence. Raw clocks from signaling,
Windows and Mac must be aligned with measured anchors before comparing gaps.

## Bounded time-probe connection repair

The signaling listener now permits sequential GET `/time` probes on one
connection, bounded to 32 requests and a three-second receive idle timeout.
It reuses the existing persistent framing and anti-pipelining checks. A reused
time-only connection cannot invoke an input/auth route or become a WebSocket;
a first-request WebSocket keeps its previous socket timeout and handling. Other
signaling HTTP requests retain connection-close behavior. Both signaling entry
points explicitly select this policy.

This removes seven TCP/TLS handshakes per normal seven-sample cohort in favor
of one, while retaining fresh request/response timestamps for every exchange.
It does not change sample count, four-timestamp validation, clock uncertainty,
input expiry, accepted-anchor rules, or any failure/rearm action. It reduces
exposure to isolated handshake loss and does not fix a common wireless outage.
Runtime regression checks cover actual TCP reuse, the 32-request bound, explicit
close, HTTP/1.0 close, route/upgrade isolation, and the first-WebSocket timeout.
A fresh 60-minute hardware run is still required.
