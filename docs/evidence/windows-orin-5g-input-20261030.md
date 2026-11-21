# Windows / Orin 5G mock input and control evidence (2026-10-30)

The runs below use an isolated test identity and mock vehicle adapter, neutral
gear and zero speed, two physical cameras and a profile limited to zero speed,
zero motor torque and zero steering. `can0` remained down. The Orin media route
was checked against its cellular interface; management traffic used a separate
connection. No VPN settings were changed. The test time proxies and isolated
signaling service are bench infrastructure, not a production clock design.
Time-sync uncertainty remained subject to the existing 25 ms limit.

## Initial native 30-minute window

The Windows native viewer and Orin vehicle binaries used source `5d1f0f0`.
The viewer's measured window was 1,800.001 seconds after initialization and
keyboard exercises; the window itself used neutral input. Both critical camera
streams and the selected SCTP transport used the designated global IPv6 host
pair over UDP. Orin's route and CAN guard did not fail. There were three
vehicle braking-wait episodes during this window: one control-link episode and
two reported critical-video episodes. `stop_sequence` remained zero, but the
braking interventions mean that uninterrupted safe-control acceptance failed.
The first control-link episode included a roughly 315 ms vehicle command
arrival gap and expired commands despite a roughly 51 ms local send interval.
The available data does not locate the delay between local dispatch and remote
receipt. For the two critical-video episodes, the sender-side camera observations
were fresh, but the receiver combined several readiness conditions into the
same signal; their individual causes were not established.

The foreground GDI submission intervals over the native session, including
initialization, had p99 of 51.3 and 50.5 ms and maxima of 283.6 and 280.7 ms.
These are not camera-to-screen or physical scan-out timings. The reported
software-result round trips had p50 39, p95 49, p99 54 and max 125 ms across
35,411 results; expired commands without results are not represented.

## Browser 30-minute window with bounded local HTTP reuse

The Windows control executable used source `934b7b8` (including the reuse and
pipeline fixes in `5d1f0f0` and `3c1de0b`); the Orin binary used `3c1de0b`.
After a separate preflight that fell back to WSS, a new session started only
after direct DataChannel control, two fresh video tracks, mock profile and
clock readiness were observed. A headed Edge browser used a dedicated test
profile with the explicit mDNS-disabling flag. The actual media candidate pair
was checked against the designated IPv6 endpoints. This does not establish
default-browser connectivity.

The new window lasted 1,800.091 seconds. All 1,792 one-second observations
showed the same connected peer, the two designated UDP host candidates and
both videos ready. Eighteen selected-pair reports had `in-progress` stats;
the peer nevertheless remained connected in those samples. Control used
DataChannel in 1,067 observations and WSS in 725. After three failed direct
control trials, this media attempt exhausted its retry budget and remained on
WSS. The vehicle log recorded six control-link braking-wait entries and six
recoveries in the window; a 50 ms telemetry poll, attached 44.9 seconds after
the start, reported five entries. The vehicle log is the authoritative count.
`stop_sequence` remained zero. The window therefore completed but did not meet
uninterrupted direct-control or no-intervention acceptance.

One-second sampled software-result round trips, grouped by command path, were
40/56/65/546 ms (p50/p95/p99/max, 1,069 DataChannel samples) and
60/75/81/94 ms (723 WSS samples). They are not one-way or capture-to-display
measurements and can miss short events. The two browser decoder tracks advanced
53,935 and 53,949 frames, with 11 and 9 reported freezes totaling 3.892 and
3.555 seconds. The corresponding native GDI statistic measures a different
stage and should not be compared as display latency.

During the browser observation no `ERR_NO_BUFFER_SPACE` was found in the
captured console log. The Windows host-wide `TIME_WAIT` sampler, started after
the window began, recorded 58 observations: min 56, median 65, max 88. A prior
run using response-per-connection observed 5,743–7,224 and 11 buffer errors.
The sequential tests and different path conditions do not prove a root cause,
but this run supports reduced local connection churn. For the first control
intervention, the local driver-to-browser send timestamp differences around
commands 4700–4899 had medians of 401 and 399.5 ms; command 4908 was 403 ms.
The offset includes the different clock domains, so these are not one-way
latencies. The absence of a relative local spike, followed by a burst of expired
commands at the vehicle, suggests delay after browser enqueue or on the receive
side; it does not identify a particular network or host as the cause. The conservative
post-response pipeline check can close a connection even when a very fast
client sent a sequential request after receiving its response; this is a
fail-closed tradeoff and clients may need to reconnect.

## Native input timing diagnosis

A separate diagnostic native run used source `3e00868` and was deliberately
stopped after approximately 284 seconds to fix a discovered input issue; it is
not a 30-minute acceptance window. Bounded, opt-in guard records showed 150
transitions to unauthorized while the combined video, clock, remote media,
foreground and direct-path readiness was true. The input would be neutralized
and a held key would require release and a fresh press. No guard records were
dropped. The same short run matched 5,457 command sequences across local
dispatch and DataChannel enqueue: p50 5 ms, p95 10 ms, p99 11 ms and maximum
43 ms; all recorded enqueue attempts succeeded. This run did not reproduce the
30-minute control-link event. Code inspection found that the main loop could compare a timestamp
captured before courier delivery with telemetry received later in the same
loop; the input worker also compared a timestamp captured before reading the
latest authorization. Follow-up commits take consistent clock snapshots,
retain the 200 ms input and 150 ms foreground heartbeat boundaries, and add
bounded guard-age diagnostics. Post-fix hardware results must be recorded
separately; these observations alone do not establish that every prior guard
transition had the same cause.

A separate controlled before-fix run held the mock W key for approximately
5.02 seconds. The vehicle accepted a positive throttle request for 3.459
seconds, then received neutral throttle while the key was still held. Its
command transition from sequence 93 to 94 coincided with a guard transition
from authorized to unauthorized even though focus, video, clock, remote
media and direct path remained ready; authorization recovered approximately
60 ms later but the held input correctly required a new release and press.
This is mock input evidence only and does not establish real-vehicle control.

In the preflight of the post-fix `41e2730` native viewer, a five-second W
hold produced 99 consecutive accepted mock throttle requests spanning 4.996
seconds, with no neutral command between the first and last. This validates
the exercised hold on this bench, not all input or network conditions. That
run also showed genuine control-link braking and WSS fallback; its full
30-minute outcome is reported below after the window ends.

## Post-fix native 30-minute window

The Windows executable was built from `41e2730` (SHA-256
`80ac34d0a5f186dfde4f1915e9c452f5cf6c7abca687067027c431c63865b13a`);
the vehicle executable was built from `3e00868`, and the isolated signaling
service used `5d1f0f0`. Later commits changed the Windows viewer and its tests,
not the vehicle control source. The post-fix input exercise preceded the
1,800.722-second neutral-command measurement window. The vehicle guard reported
no route failure and CAN remained down.

All 1,790 one-second native observations in the window reported the designated
direct UDP SCTP pair. Control path sampling recorded 470 DataChannel and 1,320
WSS observations. The vehicle recorded six control-link braking-wait entries
and recoveries in the window; `stop_sequence` stayed zero. The native client
reported six safety episodes over its entire session, including preflight.
These results do not satisfy uninterrupted direct control or no-intervention
acceptance. Sampling boundaries were aligned to Windows monotonic time using
Windows UTC/monotonic pairs; the six vehicle entries were away from the window
boundary after accounting for the measured clock offset.

Resampling software-result round trips to one sample per second produced:

| Command path | Samples | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| DataChannel | 472 | 39 ms | 46 ms | 55 ms | 264 ms |
| WSS | 1,328 | 56 ms | 73 ms | 79 ms | 104 ms |

These sampled result latencies include a feedback path, can miss tail events
and do not measure physical execution or camera-to-display latency. The full
native session's foreground GDI submission intervals had p99 53.2 and 51.3 ms,
with maxima 537.8 and 544.7 ms; these include preflight and are not browser
decoder or scan-out measurements.

For an early control-link intervention, native sequences 612 and 613 reached
Windows DataChannel enqueue 8 and 0 ms after local dispatch, but reached the
vehicle approximately 643 and 594 ms after their synchronized command times and
were rejected as expired. WSS commands around the same incident also arrived
late. This places the unexplained delay after local enqueue, but cannot separate
network and receiver scheduling. In a later WSS incident, the cloud logged a
command queued approximately 10 ms after dispatch, and the vehicle received it
approximately 404 ms after the cloud queue timestamp; this interval includes
the test-only signaling tunnel and vehicle receive path.

A 10 Hz, independently ported direct IPv6 UDP echo probe was added during the
latter portion of the window; it adds a small amount of test traffic and is not
the WebRTC control path. After bidirectional punching, 7,608 completed echo
records over 831.1 seconds had no recorded timeout and p50/p95/p99/max RTT of
23/30/37/77 ms. Pending probes at the forced end are not counted as successful
or lost. A preceding single-sided probe did not receive replies and is retained
separately; it does not prove that the established WebRTC path failed. During
one WSS braking event, nearby UDP echoes stayed in the tens of milliseconds,
but the different route and probe cadence prevent assigning the WSS delay to a
specific component. A read-only vehicle thread run-queue sampler started partway
through the window; its lack of a contemporaneous >50 ms runnable-wait record
for that event does not rule out blocking or network delay.

After the tests, the two restricted temporary cloud SSH keys were removed while
all other authorized-key bytes were preserved; temporary credentials, test
browser profile and scheduled tasks were removed. Test listeners stopped,
`can0` remained down and the cameras were released. Raw private evidence stays
on the local test host and is not included in this repository.

## Automated validation boundary

For `3e00868`, the macOS product-direct integration test once failed its
existing 80 ms `recovery did not withdraw DC` assertion. The unchanged job
passed on rerun; this is an unresolved intermittent failure, not proof of a
root cause or permission to relax the bound. For `41e2730`, the Windows native
bench, offline regressions and build/release workflows completed successfully.
Local Linux control tests passed 57/57 and the real loopback HTTP reuse,
request-bound and framing checks passed after the handler-error fix. The HTTP
server intentionally fails closed on ambiguous pipeline timing and can close a
very fast legitimate sequential connection.
