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
