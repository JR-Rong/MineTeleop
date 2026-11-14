# Windows / Orin 5G repeated mock comparison

All sessions used the isolated `test_bench_pr26` identity, mock adapter, zero
configured speed, unchanged control/time/video safety gates, and CAN DOWN. The
vehicle used the same two physical cameras at configured 960×768, 30 fps and
1,500 kb/s per stream. Windows used WLAN; Orin's selected IPv6 peer route was
checked against the independent cellular interface. SSH management and
low-rate IPv4 STUN / Windows gateway diagnostic probes were separate from the
IPv6 media and control path. No VPN settings were changed. These experiments
are not real-vehicle or physical camera-to-screen acceptance.

## Native UI, full telemetry and JSON control

`native-brake-30` used Windows viewer source `5920a7c`, vehicle runtime source
`360da70`, full telemetry and JSON control. The formal window lasted
1,800.922 s. It used five 1.2-second Space exercises at approximately
300-second intervals. The vehicle logged respectively 22, 25, 25, 22 and 23
positive brake requests in those intervals, all accepted, with requested
normalized brake 0.3. Vehicle time alignment is subject to the measured clock
uncertainty; the independent safety brake and physical hard limits were not
changed.

The vehicle recorded seven braking-wait entries: six control-link events and
one readiness-history event. The sender recorded 33,779 DataChannel and 1,469
WSS command sends; the vehicle rejected 34 expired commands and six replayed
sequence numbers. The Windows control guard had 14 not-ready/unauthorized
intervals totaling 6,921 ms, without guard batch drops. Therefore the window
failed uninterrupted-direct-control and no-intervention acceptance. Sampled
viewer DataChannel result RTTs across 33,442 results were p50/p95/p99/max
40/56/67/724 ms; these are round trips for returned software results, not
physical execution or complete one-way latency.

At one incident, several commands entered the vehicle callback within a few
milliseconds after being sent roughly 50 ms apart; the largest estimated
sender-to-callback delay among rejected commands was 489 ms. Callback mutex
wait among rejected commands was at most 2 ms. This locates that observed
excess before callback, but without a kernel capture does not divide network,
OS receive scheduling, and earlier processing. Around two incidents, the
Windows gateway ICMP diagnostic peaked at 254 and 323 ms, while the Orin-to-
cloud IPv4 STUN probe peaked at 33 and 27 ms in the respective six-second
windows. Probe traffic was separate from control and ICMP can be deprioritized;
these correlations do not identify the exact link at fault.

## Earlier web window and incomplete matched window

`web-full-30` used the same vehicle, camera and JSON/full-telemetry settings.
Its 1,800.789 s observation recorded three vehicle control-link braking entries.
All 1,793 one-second samples observed the specified IPv6 UDP host pair, while
control-path samples were 1,510 DataChannel and 283 WSS. The browser reported
15 samples without focus. Five scripted W presses did not produce nonzero
throttle because the browser correctly clamps throttle to zero when the vehicle
hard speed limit is zero. Thus this window is not a matched nonzero-input
comparison. The browser's approximately one-second sampled result RTTs were
DC p50/p95/p99/max 40/58/73/444 ms and WSS 62/90/113/295 ms. They can miss
short events and do not measure the same sampling population as the native
all-result statistics above.

A subsequent `web-brake-30` preflight used a mock-only session profile with
unchanged vehicle hard limits and safety gates. Normal service brake was set
to 98.3 bar against a 327.6 bar session maximum so a Space press requested
approximately 0.30006, matching the native 0.3. All 24 positive preflight
requests were accepted. The formal run was stopped early after approximately
10 minutes because one video element was paused for about six seconds and the
vehicle entered TIMEOUT_BRAKE; decoded RTP frame counts continued to increase,
but actual presented-frame freshness did not. Additional unplanned input was
observed around the interruption, without evidence establishing why the video
paused. The fail-closed response is retained; decoded frames alone do not
restore control. This incomplete run is not a 30-minute acceptance window.

## Final matched browser retry

`web-brake-31` completed 1,800.372 s with the same mock service-brake profile.
All 1,794 one-second samples observed the designated IPv6 UDP host pair and
an open DataChannel; 1,779 pair observations reported `succeeded` and 15
`in-progress`. The control path, however, was DataChannel in 417 samples and
WSS in 1,377. Focus, visibility and video freshness were good in every sample.
The vehicle logged six control-link braking-wait entries, all followed by
recovery, at 418.676, 420.026, 807.126, 1,187.027, 1,755.478 and 1,756.128 s.
It rejected 37 expired commands. This fails continuous direct control and
no-intervention acceptance despite the connected media pair.

The five scheduled Space exercises each produced accepted positive requested
brake commands: 23, 24, 24, 24 and 24 respectively, approximately 0.30006.
Their effective brake remained 1.0 under the existing recovery safety-brake
hold. The corresponding native run also logged effective brake 1.0 for its
positive requests. Acceptance of the requests therefore does not establish
normal unheld actuation; this remains a mock-only safety observation.

Browser one-second sampled software result RTT p50/p95/p99/max was
41/60/75/83 ms for DataChannel (418 samples) and 62/88/108/427 ms for WSS
(1,376 samples). Selecting the last native result in each one-second bucket
gives 39/57/68/436 ms (1,726 DataChannel buckets) and 63/84/316/316 ms
(75 WSS buckets). These sequential windows experienced different network
conditions, sampling can miss short tails, and WSS sample counts are very
different, so neither comparison proves one implementation generally better.
Browser sampled clock uncertainty was at most 16 ms at the controller and
22 ms at the vehicle; this is not physical camera-to-display latency.

The Windows visual aspect-fit change `859a33f` was tested separately from
the native 30-minute run, which used `5920a7c`; no 30-minute run of the later
renderer is claimed. The browser used an isolated Edge test profile with mDNS
host candidate obfuscation disabled for address verification. IPv4 diagnostic
probes added a small separate workload. These constraints and the different
Windows IPv6 addresses prevent treating the comparison as identical conditions.
