# Windows native UI mock preflight, 2026-11-01

This short preflight used the Windows viewer from `87bff3f` and the Orin mock
runtime from `26c00d0`. Windows used Wi-Fi and Orin used its independently
routed 5G interface. The viewer was restricted to the specified distinct
public IPv6 addresses, and the Orin guard checked that its pinned address was
still preferred on the cellular interface. CAN remained DOWN. Two physical
cameras were used with a zero-speed, zero-steering, zero-torque mock profile.
No VPN or safety threshold was changed.

The requested 15-second measurement lasted 18.72 seconds because the final
four-second key exercise was allowed to finish. Nineteen one-second client
observations all reported the specified direct UDP SCTP pair. Fourteen reported
the DataChannel as the logical control path and five reported WSS. Two safety
stop episodes occurred during measurement: vehicle logs recorded a critical
video event at approximately 8.8 seconds and a control-link event at 13.8
seconds. Another control-link braking event occurred before the measurement.
This run failed uninterrupted-control acceptance.

Forward, reverse and left were each exercised once. The local input revision
was correlated with respectively 78, 2 and 78 accepted vehicle commands.
All matching commands had zero requested and effective traction and steering,
as required by the mock profile. The reverse window contained only one local
input observation and two matching vehicle commands; this is not equivalent
to uninterrupted control. The other six requested key scenarios, including
brake, were not covered in this short preflight.

Both GDI submission streams had substantial local gaps: p99/max approximately
352/601 ms and 284/685 ms. The client recorded a video-health failure around
the first formal stop and a control-link failure around the second. These
measurements alone do not distinguish network delay, decode stalls, UI
scheduling or rendering. They are local submission intervals, not physical
camera-to-display latency. The later changes add decoded-callback and
callback-to-submission measurements for a subsequent run.

In the three seconds centered on the first formal stop, the vehicle rejected
11 expired commands: one on DataChannel and ten on WSS. For those commands,
sender-timestamp-to-callback estimates ranged from 220 to 339 ms; these are
cross-host estimates subject to clock uncertainty. The maximum measured
vehicle callback-to-control-mutex wait in that interval was 1 ms. This does
not identify whether the preceding delay occurred in Windows scheduling,
either network path, or vehicle scheduling before the callback.

The separate Windows and Orin startup clock probes reported uncertainties of
14 ms and 19 ms respectively; these are individual preflight samples, not a
guarantee throughout a session. No physical gamepad was connected. Raw logs
and analysis remain in the local artifact directory under `ui-preflight-01-87`.
No 30-minute result, physical CAN result, or real-vehicle acceptance is
established by this preflight.

## Second short run with all nine inputs

A second run using the same viewer and vehicle builds measured 66.25 seconds.
All 11 scheduled key windows had at least one matching accepted vehicle
command, covering the eight requested directions and brake. Ten windows had
at least 77 matches; the initial forward window had only 10. All 66 sampled
observations reported the pinned direct UDP pair and the DataChannel logical
control path. One critical-video safety stop occurred during measurement, so
uninterrupted-control acceptance again failed.

In the six seconds centered on that stop, the Windows-to-Wi-Fi-gateway probe
recorded five replies, no timeouts and a maximum of 10 ms. This single probe
does not exclude a problem later in the network path or a scheduling delay.
The two GDI submission streams had p99/max gaps of approximately 145/555 ms
and 121/379 ms. The vehicle runner reported 24 polls matching a FrameForge
process but did not capture process identities; the other workload and its
impact are unconfirmed. No other task was paused. Raw evidence remains in
`ui-preflight-03-87` in the local artifact directory.

## Session log export preflight

With no control session open and the isolated mock vehicle online, the native
viewer from `87bff3f` displayed two authorized completed sessions and produced
a readable ZIP for `test_bench_pr26`. The ZIP manifest marked the vehicle,
cloud and controller sources partial; it is not a complete three-end export.
The automated test selected the wrong Windows dialog input and saved to the
default Documents location. After verifying the file's creation time, mock
identity and SHA-256, it was moved without overwriting to the dedicated
`ui-log-preflight-01/session.zip` test directory. Two subsequent attempts to
automate a custom save location did not identify the native file-name control;
custom-path saving remains unverified, not a demonstrated client failure.

## First 30-minute attempt

The first long attempt used Windows viewer `b842b42` and the same Orin mock
runtime. It ended after 1,261.06 seconds because a safety stop occurred; it
did not pass the requested 30-minute uninterrupted window. All 1,254 client
observations reported the pinned direct UDP pair. The logical control path
was DataChannel in 1,249 observations and WSS in five. Two vehicle braking
waits were recorded during the measurement (`control_link` and
`readiness_history`); the client counted one stop episode. Of 210 scheduled
input windows, 209 matched accepted vehicle commands. The unmatched left-key
window overlapped the failure. All nine scenarios were exercised, and the
recorded effective mock traction and steering remained zero.

During the three seconds centered on the first vehicle stop, the gateway probe
had one timeout and a maximum successful RTT of 113 ms. The vehicle rejected
12 expired commands, four from DataChannel and eight from WSS; the estimated
sender-to-callback delays for those commands were 204–307 ms, subject to
cross-host clock uncertainty. The maximum callback-to-control-mutex wait was
1 ms. This is consistent with a delay before or at callback entry and shared
access-path trouble, but does not establish the exact cause or network segment.

The two decoded-sample callback interval distributions had p99/max values of
approximately 52/319 ms and 52/323 ms. Decoded callback to foreground GDI
submission had p99/max of approximately 4.5/16.7 ms and 5.1/17.3 ms. These
local measurements do not establish capture-to-display latency, but show that
the large gaps in this attempt arose before the measured UI submission stage
or in scheduling shared with it. Raw evidence and the complete analysis remain
in the local artifact directory under `ui-long-01-b842`.

## Second 30-minute attempt

Viewer `1d707e1` ran for 186.76 seconds before a critical-video stop and a
subsequent control-link stop. All 186 sampled observations showed the pinned
direct pair and DataChannel logical path. The 31 scheduled input windows all
matched accepted mock commands and covered all nine scenarios. The client
terminated when the vehicle invalidated the control profile. The gateway probe
had one timeout in the six-second window around the first stop; the maximum
successful RTT there was 12 ms. Estimated sender-to-vehicle callback delay
peaked at 439 ms across the run, subject to cross-host clock uncertainty;
callback-to-control-mutex wait peaked at 1 ms. These observations do not
identify the responsible network segment or establish an uninterrupted pass.
Raw evidence is retained under `ui-long-02-1d707` in the artifact directory.

## Focus-loss and profile confirmation preflight

With viewer `4bf10ce`, a deliberate focus loss withdrew input and produced a
vehicle safety stop. The viewer stayed open and offered manual profile
confirmation. The simulated operator selected it; the vehicle logged a new
request, sequence 7, as accepted. It was then cleared while the vehicle was in
`TIMEOUT_BRAKE`, and later commands were rejected as `session_profile_not_ready`.
The test did not recover active control. Direct media remained selected in
the sampled observations, while the logical control path alternated between
DC and WSS. The failure does not justify relaxing the safety state or treating
the accepted profile request as evidence of recovery. Raw evidence is retained
under `ui-profile-01-4bf` in the local artifact directory.

## Third 30-minute attempt

Viewer `4bf10ce` ran 264.21 seconds before a control-link safety stop. All
263 sampled observations retained the pinned direct UDP pair; the control
path was DC for 259 and WSS for four observations. All nine input scenarios
were covered, and 43 of 44 scheduled input windows matched an accepted
vehicle command; the final unmatched window overlapped the stop. There was
one video-not-ready sample and no clock-threshold violation. All accepted
mock traction and steering values remained zero.

In the failure window, the last accepted DC command arrived approximately
540 ms before the vehicle reported its stop. Several WSS commands then
arrived 264–466 ms after the sender timestamp, allowing for cross-host clock
uncertainty; callback-to-control-mutex waits for those commands were at most
1 ms. A one-second Windows-to-Wi-Fi-gateway probe timed out in the same
window. This correlates with a disruption on a shared access path or in host
scheduling, but cannot isolate its cause. The two decoded-frame callback
intervals peaked at about 608 and 426 ms; decoded callback to GDI submission
peaked at about 12.4 and 11.6 ms. These are not capture-to-display latency.
Raw evidence is retained under `ui-long-03-4bf` in the local artifact directory.

## Fourth 30-minute attempt

Viewer `4bf10ce` ran 493.98 seconds before the harness observed a WSS control
selection and stopped the run. The pinned direct UDP pair remained selected
in all 491 observations; 490 sampled the DC logical path and one sampled WSS.
All 82 scheduled input windows matched accepted mock commands, covering all
nine scenarios. The once-per-second client observations missed the final
stop, but the final render summary counted one and the vehicle recorded a
`control_link` braking wait. The requested uninterrupted run therefore did
not pass. The vehicle recorded DC commands up to approximately 2.1 seconds
old; the Windows-to-gateway probe timed out approximately 1.8 seconds before
the vehicle event. Clock uncertainty and the one-second probe interval limit
attribution. Raw evidence is retained under `ui-long-04-4bf` in the local
artifact directory.

## Fifth 30-minute attempt

Viewer `a05fe67` ran 1,794.78 seconds, about five seconds short of the target,
before a control-link safety stop. All 1,785 sampled observations retained
the pinned direct UDP pair; 1,781 sampled DC control and four sampled WSS.
Of 299 scheduled input windows, 298 matched accepted vehicle commands; the
unmatched reverse window overlapped the failure. All nine scenarios were
covered, all accepted mock traction and steering values were zero, and there
was no clock-threshold violation. The gateway probe reported 355 ms near the
stop. This run did not pass 30-minute acceptance. Raw evidence is retained
under `ui-long-05-a05` in the local artifact directory.

## Exact-head focus and control preflights

In a deliberate focus-loss test with viewer `2abca39`, the vehicle entered
hard timeout. The viewer kept ordinary input inhibited, disabled profile
confirmation and continuously displayed the instruction to disconnect and
follow the safety procedure. The vehicle recorded only its initial profile
request. The injected stop is not a long-duration acceptance result.

The first long attempt with this build ran 235.53 seconds before a control-link
braking wait. All 234 observations retained the pinned direct pair, while the
control path sampled DC 233 times and WSS once. All 39 input windows matched
vehicle-accepted commands and covered the nine scenarios; all accepted mock
traction and steering values were zero. This run did not pass 30-minute
acceptance. Raw evidence is retained under `ui-profile-03-2ab` and
`ui-long-06-2ab` in the local artifact directory.

The next exact-head run lasted 793.35 seconds and recorded two control-link
braking-wait events. All 132 input windows matched, and all 789 one-second
observations retained the direct pair and DC logical path. Among accepted
result records, 14,571 reported the DC copy arriving first and 640 reported
the WSS mirror arriving first. The dual-carrier run therefore does not prove
exclusive direct delivery. Evidence is under `ui-long-07-2ab`.

An attempted compact-control comparison was stopped after confirming that it
actually sent JSON. The dual-control signed extension was not representable by
the original compact format, so the existing exact-reconstruction check safely
fell back. That interrupted run is not a compact or 30-minute result; its
records remain under `ui-compact-01-invalid`.

## Negotiated compact dual-control attempts

With the dual-control extension explicitly negotiated, the vehicle and viewer
at product commit `2e92f73` used compact control messages, usually 76–78
bytes rather than approximately 963–966 bytes for the previous JSON payload.
Full telemetry remained enabled. A separate 18.7-second compatibility run with
the newer vehicle and older viewer fell back to JSON and had no observed
vehicle braking event; this short run does not establish long-run reliability.

The first compact run ended at 1,225.02 seconds with one vehicle
`control_link` braking wait. All 204 scheduled input windows matched accepted
zero-limited mock commands, and all nine scenarios were covered. All 1,219
client observations retained the pinned direct UDP pair, but two sampled the
WSS logical control path. Accepted result records reported 23,149 direct
copies and 605 WSS mirrors arriving first. The application-result RTT was
40/55/65/423 ms at p50/p95/p99/max. The last accepted command aged to 502 ms;
nearby direct and WSS copies arrived late, with some rejected as expired. A
gateway ICMP probe timed out near the event. Neither the probe nor this timing
isolates the delayed segment. The run did not meet 30-minute acceptance.
Evidence: `ui-compact-02-2e` in the local artifact directory.

The second compact run ended at 432.07 seconds with one vehicle
`control_link` braking wait. All 72 input windows matched, with eight of each
scenario. All 430 client observations retained the direct pair; one sampled
WSS as the logical control path. The sender-to-vehicle callback delta reached
580 ms, subject to the two hosts' clock uncertainty. No gateway probes timed
out in this run; their one-second spacing cannot rule out a shorter disruption.
The run did not meet 30-minute acceptance. Evidence: `ui-compact-03-2e`.

The viewer subsequently added handling for the vehicle's authenticated
`control_input_rearm_required` status, preserving the confirmed gear and
braking while requiring direction and gamepad input to return to neutral. The
two runs above used the earlier viewer and do not validate this change. Unit
tests cover the release and braking interlocks; a new Windows/Orin run is
required to evaluate the revised build.

The revised viewer `bb514d0` ran for 1,309.93 seconds before one vehicle
`control_link` braking wait, so this attempt also failed the requested
30-minute acceptance. All 218 input windows matched and covered all nine
scenarios. All 1,303 client observations retained the pinned direct UDP pair;
one sampled the WSS logical path. Accepted result records contained 24,016
direct copies and 1,443 WSS mirrors arriving first, plus two WSS results
without a reported receiving path. Application-result RTT p50/p95/p99/max was
40/53/61/342 ms. Accepted traction and steering remained zero.

The vehicle had just accepted a fresh command when it reported the braking
wait, so its snapshot does not establish a 500 ms gap immediately before the
event. Earlier packets in that period had arrived in repeated bursts, with
some control commands rejected as expired. In a roughly 3.5-second window
around the event, 181 matching outbound DTLS headers were found at the Windows
GStreamer `nicesink` probe and Orin kernel receive capture; 14 differed by
over 100 ms, with a maximum of about 344 ms after applying measured clock
offsets. The two clock uncertainty bounds and the probe placement limit that
comparison. Encrypted DTLS record identities cannot identify which records
contained control commands. Capture reported no packet-socket drops or missing
kernel timestamps; the application trace reported no dropped DTLS records.
Windows gateway probes near the event were 142–143 ms, while Orin-to-cloud
probes were approximately 21–25 ms. These observations narrow the delayed
path to the span between the Windows sender probe and Orin kernel for those
records, but do not isolate a particular host, wireless link or carrier.
Evidence: `ui-compact-04-bb` in the local artifact directory.

A repeat of `bb514d0` ended at 1,212.56 seconds with one vehicle
`control_link` braking wait, and therefore also failed acceptance. All 202
input windows matched and covered all nine scenarios. The pinned direct pair
was present in all 1,206 observations; one sampled WSS as the logical control
path. Accepted results reported 22,397 DC copies and 1,134 WSS mirrors first;
application-result RTT p50/p95/p99/max was 41/61/64/457 ms. At the vehicle
event, the last valid command had aged 526 ms.

Within the roughly 3.5-second event window, 46 of 52 outbound DTLS records
were matched to Orin kernel arrivals; ten matched records differed by more
than 100 ms, up to about 443 ms after applying clock offsets. No packet-socket
drops or missing kernel timestamps were reported, but six sender records did
not match, so that gap cannot be classified from this capture alone. The
endpoints' clock uncertainty and encrypted payload boundary remain relevant.
Evidence: `ui-compact-05-bb` in the local artifact directory.

A further `bb514d0` attempt ended at 420.74 seconds, with two vehicle
`control_link` braking waits. All 70 input windows matched and covered all
nine scenarios; the direct pair remained selected in all 419 observations,
three of which sampled the WSS logical control path. Application-result RTT
p50/p95/p99/max was 39/52/61/351 ms. Around the first wait, all 154 outbound
DTLS records in the selected time window matched Orin kernel records; ten
showed more than 100 ms between probes, up to approximately 304 ms after clock
offset correction. No packet-socket drops or missing kernel timestamps were
reported. These encrypted records cannot be mapped to individual control
commands. This attempt failed acceptance. Evidence: `ui-compact-06-bb`.

The next `bb514d0` attempt ended at 1,609.65 seconds with one vehicle
`control_link` braking wait. All 268 input windows matched and covered all
nine scenarios. The pinned direct pair remained present in all 1,602 sampled
observations; one sampled WSS as the logical path. Accepted results reported
30,449 direct copies and 839 WSS mirrors first. Application-result RTT
p50/p95/p99/max was 40/54/62/348 ms. Around the braking event, 169 selected
outbound DTLS records matched kernel arrivals, with eight over 100 ms and a
maximum of about 305 ms after clock offset correction. The application DTLS
trace recorded two drops across the run, so it is not complete evidence of
every packet; the packet-socket capture reported zero drops. This attempt
also failed acceptance. Evidence: `ui-compact-07-bb`.

Another `bb514d0` attempt ended at 252.73 seconds with one vehicle
`control_link` braking wait. Forty-one of 42 input windows matched; one
forward-right window did not, although all nine scenarios had at least one
match. The direct pair remained present in all 252 observations and four
sampled the WSS logical path. Application-result RTT p50/p95/p99/max was
48/66/73/1913 ms. The scoped Orin runtime did not terminate within the test
runner's eight-second shutdown grace and the runner killed its own process;
CAN remained down and the next run independently checked camera availability.
This attempt failed acceptance. Evidence: `ui-compact-08-bb`.

The next `bb514d0` attempt ended at 870.82 seconds with one vehicle
`control_link` braking wait. All 145 input windows matched and covered all
nine scenarios. All 867 client observations showed the pinned direct pair
and the DC logical control path; the vehicle event nevertheless invalidated
the acceptance window. Accepted results reported 16,488 direct copies and
394 WSS mirrors first. Application-result RTT p50/p95/p99/max was
40/53/61/440 ms. This attempt failed acceptance. Evidence: `ui-compact-09-bb`.

Input-window matches in the runs above mean that the native key mask was
observed locally and an accepted vehicle command carried the corresponding
intent sequence. The isolated profile fixes both speed and steering limits at
zero, so the actual transmitted traction and steering values are zero. These
matches do not prove that the vehicle received nonzero directional commands;
brake values remain testable under the mock profile. The zero limits are not
relaxed for this experiment.

The following `bb514d0` attempt stopped after 390.37 seconds with one vehicle
`control_link` braking wait. It also used the zero-speed, zero-steering profile;
the accepted requests carried zero traction and steering, while brake requests
were present. It did not complete the requested window. Evidence:
`ui-compact-10-bb`.

With the `1061658` WSS-owned DC mirror code, a further isolated mock attempt
could not establish ICE/DTLS during the 120-second startup deadline; it never
entered the measurement window. During a separately authorized temporary
Wi-Fi adapter power-setting experiment, endpoint-bound simultaneous UDP probes
were received by Orin but not by Windows for two tested Orin IPv6 addresses.
After restoring the original setting, a newly allocated Windows address passed
a bidirectional 73/73 probe; another probe to a different address was one-way,
and a newly allocated temporary Windows address passed 72/72. This small,
non-simultaneous sample does not isolate the adapter setting from address,
neighbor, router or network state. The original setting was restored and
verified; no VPN or safety threshold was changed. Evidence: `ui-compact-11` and
the associated `udp11` through `udp16` probe artifacts.

Using the bidirectionally reachable temporary Windows address, another attempt
entered the direct mock measurement but stopped at 12.18 seconds after a
braking wait. Two input windows matched local key masks to accepted, zero-limited
vehicle intent sequences. At one braking event the last valid control command
was only 5 ms old; this is not evidence of an 800 ms command outage. Evidence:
`ui-compact-12-106`.

The following attempt stopped after 163.57 seconds when critical video became
stale. All 27 input windows matched local key masks to accepted zero-limited
vehicle intent sequences, and three brake windows contained nonzero brake
requests. The last valid command was 98 ms old at the critical-video wait.
Decoded callback inter-frame gaps had p99 approximately 180--181 ms and maxima
approximately 276--279 ms on the two streams. This does not yet establish
whether the gap arose in the network, jitter buffer or decoder. Evidence:
`ui-compact-13-106`.

The next attempt stopped after 510.68 seconds with a `control_link` braking
wait; all 85 input windows had local key-mask and accepted zero-limited intent
sequence matches, including nine nonzero-brake windows. The last valid command
was 529 ms old at the first measured braking wait and 1,129 ms at a later
wait. The two decoded callback streams each had a maximum gap of about 549 ms.
A one-hertz Windows gateway probe near the first wait rose from 2--3 ms to
127 ms, while the vehicle-to-cloud probe was 21--22 ms immediately before it;
these sparse ICMP samples are correlation, not proof of the failing segment.
In a seven-second window around the first wait, 255 of 258 outbound encrypted
DTLS records could be matched unambiguously to vehicle kernel arrivals; five
had an estimated nicesink-to-kernel interval above 100 ms, with a maximum of
about 335 ms after clock correction. Three unmatched records are not proof of
loss, and encrypted records cannot be attributed to individual commands.
Accepted vehicle-result RTT p50/p95/p99/max was 39/52/59/97 ms, which excludes
the commands for which no result arrived. Evidence: `ui-compact-14-106`.

Another run stopped after 432.23 seconds with a control-link braking wait and
last valid command age of 511 ms. All 72 input windows had a local key-mask and
accepted zero-limited intent sequence match, with eight nonzero-brake windows.
Both decoded callback streams had a maximum inter-frame gap near 1,118 ms.
Within seven seconds of the braking event, 279 of 286 encrypted DTLS records
matched unambiguously between Windows nicesink and the vehicle kernel; nine
were above 100 ms, with a clock-corrected maximum of about 323 ms. Unmatched
records are not proof of loss and cannot be attributed to individual commands.
Evidence: `ui-compact-15-106`.

With receiver timing export enabled, the next attempt stopped after 1,603.25
seconds on a control-link braking wait. All 267 input windows matched local
key masks to accepted zero-limited intent sequences, including 29 windows with
nonzero brake requests. Every one of 1,595 measurement observations showed the
pinned direct pair and logical DC path; the vehicle event still failed the
acceptance. The vehicle snapshot had a fresh command 6 ms old, but immediately
preceding commands included multiple DC deliveries rejected as expired at
229--331 ms. Accepted result RTT p50/p95/p99/max was 39/52/60/92 ms; missing
or rejected results are outside that distribution.

The two tracks' receiver-local, PTS-matched jitter-input-to-decode p99 values
were 1.33 and 1.38 ms. The maximum frame gaps at jitter input were 353.7 and
353.9 ms, and the decoded gaps were similar. This locates those particular
gaps before the recorded jitter input, but cannot distinguish network delay,
packet loss, SRTP processing or sender behavior. In a seven-second window
around the braking wait all 254 observed outbound DTLS records matched vehicle
kernel arrivals; six intervals exceeded 100 ms, with a clock-corrected maximum
near 307 ms. The application DTLS trace dropped one record across the run;
encrypted records cannot identify individual control commands. Evidence:
`ui-compact-16-c1`.

An isolated bandwidth experiment changed only the vehicle's target video
bitrate from 1,500 to 1,000 kbps per camera; both remained 960×768 at 30 fps
and all safety gates were unchanged. It stopped after 942.49 seconds on a
control-link braking wait, with the last valid command 499 ms old. All 157
input windows matched local key masks to accepted zero-limited intent sequences,
including 17 nonzero-brake windows. The two receiver jitter-input frame gaps
reached approximately 523--530 ms; matched jitter-input-to-decode p99 remained
approximately 1.3 ms. In the seven seconds around the wait, 361 of 367 observed
outbound DTLS records matched kernel arrivals; nine intervals exceeded 100 ms
and the clock-corrected maximum was about 371 ms. Missing matches do not prove
packet loss, and encrypted records cannot identify control commands. This one
experiment does not prove that a lower bitrate improves or worsens stability,
and its image quality was not evaluated. Evidence: `ui-compact-17-c1-1000`.

Separately, IPv4 STUN queries sent through the explicitly bound Windows WLAN
and Orin cellular interfaces used the same socket per endpoint against two
servers. Windows reported the same public mapping; Orin reported different
mapped ports for the two destinations. A 25-second simultaneous IPv4 UDP
hole-punch attempt using the same STUN-bound sockets received no matching peer
packets at either end. This is a failed raw UDP experiment for this particular
network and candidate strategy, not proof that IPv4 ICE or all IPv4 networks
cannot connect. The test did not modify VPN configuration.

Returning to 1,500 kbps per camera with Windows build `606e5c2`, the next run
stopped after 96.64 seconds on a critical-video braking wait. The last valid
control command was 60 ms old. Fifteen of 16 input windows matched local key
and accepted zero-limited intent sequence; one reverse-left window did not
match. One brake window contained nonzero brake. Receiver-local jitter-input
frame gaps reached approximately 229 ms on both tracks, while the
decoded callback maximum reached approximately 250--327 ms. The hardware
remained isolated mock with CAN down. Evidence: `ui-compact-18-606`.

The next `606e5c2` run completed 1,801.18 seconds at 1,500 kbps per camera.
All 1,792 measurement observations showed the pinned direct UDP pair, logical
DC path, ready video and trusted clock; the maximum sampled uncertainty was
17 ms. The client reported no safety-stop episodes and no vehicle braking-wait
event was found in the measurement window with a 100 ms boundary margin. All
300 input windows matched local key masks to accepted zero-limited vehicle
intent sequences, and all 33 brake windows contained nonzero brake requests.
Accepted vehicle-result RTT p50/p95/p99/max was 40/53/61/94 ms; the accepted
result observations attributed 33,494 first copies to DC and 1,568 to WSS.
Missing or rejected results are not represented in that RTT distribution.

The two tracks' receiver-local matched jitter-input-to-decode p99 values were
1.29 and 1.34 ms. Their maximum jitter-input frame gaps were approximately
113 and 111 ms; maximum decoded gaps were approximately 200 and 179 ms.
These are local stage measurements, not camera-to-display latency. This is a
passing isolated zero-speed mock window for the tested binary and network,
not real-vehicle acceptance, physical gamepad validation, or proof that the
path is stable in other windows. Evidence: `ui-compact-19-606`.

The subsequent Windows `3503bf8` run stopped after 138.17 seconds on a
critical-video braking wait. All 138 sampled control paths remained direct DC,
and the last valid control command was 66 ms old when the vehicle logged the
wait. All 23 scheduled input windows matched local key masks with accepted
zero-limited intent sequences; both brake windows contained brake requests.
The maximum receiver-local decoded gaps were approximately 200 and 203 ms,
and foreground submission gaps approximately 198 and 202 ms. Those aggregate
gaps do not establish why the native video gate withdrew readiness: it also
responds to loss of window usability and invalid frame or clock evidence.
The vehicle did not report a camera fault at the wait. The subsequent change
adds separate diagnostic counts for the gate's withdrawal causes without
changing its thresholds or acceptance behavior. Evidence: `ui-compact-20-3503`.

A further `3503bf8` attempt stopped after 168.46 seconds on a control-link
braking wait. All 28 scheduled input windows matched, with three brake
windows containing nonzero brake requests. The direct UDP pair was present in
all 168 observations, but the logical control path was WSS in two of them.
Both receiver-local jitter-input frame gaps reached approximately 526 ms.
Around the failure, 358 DTLS records could be matched unambiguously from the
Windows outgoing trace to the Orin kernel capture; seven approximate corrected
delays exceeded 100 ms, with a maximum of 366 ms, subject to clock uncertainty.
Encrypted records do not identify individual commands, unmatched records are
not a packet-loss measurement, and the common gap does not establish which
network segment or sender caused it. Evidence: `ui-compact-21-3503`.
The one-second Windows-to-Wi-Fi-gateway probe around the wait reported
113 ms, versus 2--3 ms on adjacent probes. This is correlation with the
shared first hop, not proof of where all affected packets were delayed.

The next `3503bf8` attempt stopped after 433.05 seconds on a control-link
braking wait. The pinned UDP pair remained direct in all 430 observations,
while the logical path was WSS in four observations. Of 72 scheduled input
windows, 71 matched; the final brake window had no local key sample or
accepted intent. Seven earlier brake windows had nonzero brake requests.
At the vehicle wait, the last valid command was 63 ms old, but the incoming
command explicitly reported the control link and operator input not ready.
The driver trace shows several result RTTs over the existing 200 ms bound,
followed by transport selection that withdraws readiness until the new
authority is confirmed. In the surrounding seven-second capture, all 333
unique matched DTLS records had approximate sender-to-kernel delays; 21
exceeded 100 ms and the maximum was 276 ms, subject to clock uncertainty.
This does not identify the cause of every delayed result. The run failed the
uninterrupted-control acceptance. Evidence: `ui-compact-22-3503`.

Windows build `a60609e` stopped after 427.12 seconds on a critical-video
braking wait; the selected control path remained DC in all 426 observations.
The new diagnostics counted two stale-track withdrawals and zero unusable,
invalid-frame, replacement, explicit-fault or clock withdrawals. The last
recorded received and decoded frame on both tracks was about 550 ms before
the native readiness withdrawal; the finite-window maximum between two
recorded decoded frames does not include that final open-ended gap. At the
vehicle wait, the last valid control command was 47 ms old, and both lanes
reported capture and encoded samples under 100 ms old without camera faults.
This narrows the interruption to the path after those vehicle-side observations
and before receiver decode, but does not isolate sender queue, network or
receiver transport. Of 71 scheduled input windows, 70 matched; the final
reverse-right window did not. Seven earlier brake windows contained nonzero
brake. The run failed acceptance. Evidence: `ui-compact-23-a606`.

A further `a60609e` attempt stopped after 114.85 seconds on another
critical-video braking wait. Diagnostics again counted two stale-track
withdrawals, with no window-usability or clock withdrawal. All 114 sampled
paths were direct DC, all 19 scheduled input windows matched and both brake
windows had nonzero brake. Accepted result RTT p99 was 69 ms; it excludes
missing or rejected results. The run did not satisfy the 30-minute gate.
Evidence: `ui-compact-24-a606`.

The Windows gateway/resource probe did not start in attempts 23 and 24:
its script name was changed when the viewer version was updated. Their
gateway RTT and resource usage are therefore unavailable. The same issue
affected attempt 25; its probe was started after the safety stop, so it does
not cover the measurement or its failure.

At 1,000 kbps per camera, `a60609e` attempt 25 stopped after 1,074.65
seconds. The diagnostic counters showed two unusable-track withdrawals and no
stale-track withdrawals; the recorded guard showed the viewer was not the
foreground window. The vehicle entered critical-video braking wait, as
required by the existing focus and freshness interlock. All 179 input windows
matched and 19 brake windows contained nonzero brake requests. The sampled
logical path remained DC, and the accepted-result RTT p99 was 60 ms; the RTT
omits missing or rejected results. The interruption's foreground owner was
not recorded, and this run cannot prove that reducing bitrate fixes network
tail latency. Evidence: `ui-compact-25-a606-1000`.

Attempt 26, also 1,000 kbps per camera, stopped after 277.27 seconds on a
control-link braking wait; all 46 input windows matched and five brake windows
contained nonzero brake. In 276 observations the logical path was DC and the
selected UDP pair was direct. The Windows foreground monitor still showed the
viewer in front at the vehicle wait; it observed a different foreground only
after the viewer ended. A gateway probe started just before the wait timed
out at its 400 ms bound, with adjacent one-second samples at 2--3 ms. In the
surrounding seven-second DTLS window, 242 records matched unambiguously and
five approximate corrected delays exceeded 100 ms, with a maximum of 310 ms.
An ICMP timeout is not proof of where every delayed media or control packet
was queued, and matched encrypted records do not identify commands. Evidence:
`ui-compact-26-a606-1000`.

Attempt 27 did not enter the measurement window: the vehicle created an
offer, received an answer and a remote IPv6 host candidate, but the native
viewer never reached authenticated direct control within its startup bound.
The scoped Orin capture recorded no DTLS records for the pinned pair. A
subsequent simultaneous raw IPv6 UDP probe using the same endpoint addresses
returned all 72 Windows-sent matching probes. That short probe does not prove
ICE or control stability; the preliminary ICMP probes timed out and should
not be used as evidence that UDP was unavailable. Evidence:
`ui-compact-27-a606-1000-invalid`.

Attempt 28 entered the measurement window but stopped after 24.83 seconds;
the vehicle recorded two control-link braking waits. The selected media pair
remained direct in 25 observations, while four observations used WSS as the
logical control path. Three of four scheduled input windows matched; the
interrupted right-key window had no local sample or accepted intent. Gateway
probes near the waits increased from 2--3 ms to 105 and 240 ms before
returning to 3 ms. All 344 uniquely matched DTLS records in the surrounding
seven-second window were accounted for, with 29 approximate corrected
sender-to-kernel delays above 100 ms and a maximum of 334 ms. These concurrent
observations do not locate all queueing to the Wi-Fi link. The 1,000 kbps
trial did not eliminate the control-path interruption. Evidence:
`ui-compact-28-a606-1000`.

Attempt 29 returned to 1,500 kbps per camera and stopped after 192.90 seconds
on a critical-video braking wait. The native diagnostic counted two stale
track withdrawals; the last recorded decoded frame on each track preceded the
withdrawal by approximately 508 and 542 ms. All 191 sampled paths were direct
DC. Thirty-one of 32 scheduled input windows matched; the interrupted
forward-left window had no local key sample or accepted intent, while three
brake windows contained nonzero brake. This run failed uninterrupted-control
acceptance. Evidence: `ui-compact-29-a606-1500`.

Attempt 30 stopped after 1,272.70 seconds on a control-link braking wait.
All 212 scheduled input windows matched, including 23 brake windows with
nonzero brake. The selected media pair was direct in all 1,267 observations;
four observations used WSS as the logical control path. Around the vehicle
wait, a gateway probe reached 285 ms and the next timed out at 400 ms, with
adjacent probes around 2--3 ms. All 371 unique matched DTLS records in the
surrounding seven-second window were accounted for, with 19 approximate
corrected sender-to-kernel delays above 100 ms and a maximum of 339 ms. This
correlation does not identify the delay of an individual encrypted command
or locate all queueing. Evidence: `ui-compact-30-a606-1500`.

Attempt 31 used Windows `a60609e` and the first Orin build with mock RTP
output diagnostics, `6548d7f`. It stopped after 96.73 seconds on a
critical-video wait; all 96 sampled logical paths were DC and 15 of 16 input
windows matched. At the vehicle's wait snapshot, the last RTP output event
was 9 and 10 ms old for the two cameras; camera capture and encode snapshots
were also fresh, and the last accepted command was 51 ms old. The client
reported two stale-track events and no window-usability or clock faults.
This single snapshot proves the diagnostics were connected and the packers
were outputting near the vehicle's observation, but does not prove the
packers were continuous during the prior receive-side gap. Evidence:
`ui-compact-31-a606-6548`.

Attempt 32 used Orin `6193b9b`, with a cumulative maximum interval between
RTP payloader output events. It stopped after 294.56 seconds on a critical
video wait; the native client counted two stale tracks and all 49 input
windows matched, including five brake windows. Through the vehicle wait, the
maximum payloader-output event gaps were 52 and 48 ms, and each last output
was 11--16 ms old; the last accepted command was 47 ms old. The most recent
receiver-local matched marker and decoded observations for each track were
about 550 ms before the native withdrawal. The sender probe observes output
from the payloader, not packet transmission; receiver marker records exist
only for observations correlated through the receiver pipeline. This narrows
the interruption to after sender payloader output and before fresh decoded
frame evidence, without proving which queue, network segment or receiver
component caused it. Evidence: `ui-compact-32-a606-6193`.

Attempt 33 stopped after 492.17 seconds on a control-link wait, with the
vehicle's last valid command 500 ms old at its snapshot. All 82 input windows
matched, including nine brake windows. The logical path was DC in 489 of 490
observations and WSS in one; the selected media pair stayed direct. The
Windows gateway probe timed out at 400 ms near the wait, with adjacent
samples around 2--13 ms. At the vehicle wait the cumulative RTP output event
gap maxima were 54 and 46 ms. In the surrounding seven seconds, 323 encrypted
DTLS records matched unambiguously, eight approximate corrected delays were
over 100 ms and the maximum was 353 ms. These observations cannot identify an
individual command or conclusively assign all delay to Wi-Fi. Evidence:
`ui-compact-33-a606-6193`.

Attempts 34 and 35 stopped after 132.12 and 258.92 seconds respectively on
critical-video waits. All 22 and 43 scheduled input windows matched, with
two and four nonzero brake windows. The client recorded two stale tracks in
each run. At the vehicle waits, cumulative RTP payloader-output event gaps
were 43/45 ms and 46/43 ms, and the last output events were at most 19 ms and
4 ms old respectively. These probe timings do not attest network delivery or
monitor scan-out. Attempt 35's inbound DTLS capture had one application-trace
drop; no packet-loss conclusion is drawn from it. Evidence:
`ui-compact-34-a606-6193`, `ui-compact-35-a606-6193`.

Attempt 36 stopped after 486.91 seconds on a control-link wait. Eighty of 81
scheduled input windows matched; the interrupted last brake window did not
match, and eight brake windows contained nonzero brake. The logical path was
DC in 480 observations and WSS in five. Near the wait, a Windows gateway probe rose to
262 ms, while adjacent samples were 2--4 ms. Of 129 unambiguously matched
encrypted DTLS records in the surrounding window, 22 had approximate
clock-corrected sender-to-kernel delays over 100 ms, with a maximum of 311 ms.
This correlation cannot identify individual commands or isolate a network
segment. This run did not satisfy uninterrupted-control acceptance.
Evidence: `ui-compact-36-a606-6193`.

Attempt 37 used Windows `6a8fd48`, which adds bounded, opt-in mock tracing of
RTP frame-ending markers at jitterbuffer input. It stopped after 6.07 seconds
on a critical-video wait. Both tracks' last marker and decoded observations
preceded the client withdrawal by approximately 503 ms. At the vehicle's
wait snapshot, the last payloader-output events were 21 and 20 ms old. A
large earlier sender-output gap had already occurred before measurement and
prevents using the cumulative maximum to establish continuity during this
particular wait. Marker absence is not proof of packet absence or a complete
frame's loss, and payloader output is not proof of network delivery. The
trace dropped no markers. This run narrows the observed interruption without
identifying a network segment or proving a cause. Evidence:
`ui-compact-37-6a8-6193`.

Attempt 38 used the same mock-instrumented builds and stopped after 714.42
seconds on a control-link wait. All 711 sampled logical paths were DC and the
selected media pair was direct; WSS nevertheless delivered the first copy of
782 accepted results, compared with 13,038 over DC. Of 119 scheduled input
windows, 118 matched; the final reverse window was interrupted by the stop,
and 13 brake windows had nonzero brake. A Windows gateway probe timed out at
400 ms near the wait, with adjacent probes around 2--4 ms. In the surrounding
window, 506 encrypted DTLS records matched unambiguously, with 13 approximate
corrected sender-to-kernel delays above 100 ms and a maximum of 501 ms.
These are correlated observations, not attribution of a specific command or
network segment. Evidence: `ui-compact-38-6a8-6193`.

Attempt 39 stopped after 456.85 seconds on a control-link wait. All 76 input
windows matched, including eight with nonzero brake. The logical path was DC
in 454 observations and WSS in one; the selected pair remained direct.
First-arriving accepted copies used DC 8,359 times and WSS 507 times. Gateway
probes near the wait rose to 110 and 98 ms from a usual 2--3 ms; they did not
time out. The surrounding 272 unambiguously matched encrypted DTLS records
included 11 approximate corrected delays over 100 ms, with a maximum of
267 ms. These samples do not identify the delay of a particular command or
fully explain the stop. The vehicle trace shows a roughly 316 ms gap between
the last accepted command and the next batch; two expired commands in that
batch were rejected by the transport fence, and later fresh commands arrived
after the control state had degraded. Reordering within that batch alone
cannot fill the preceding gap. Evidence: `ui-compact-39-6a8-6193`.

Attempt 40 stopped after 144.29 seconds on a critical-video wait; all 24 input
windows matched, including two with nonzero brake. All 143 sampled logical
paths were DC, while WSS delivered 142 accepted copies first. At the wait,
vehicle RTP payloader-output event maximum gaps were 46 ms for each camera,
and each last event was 4 ms old. The last receiver jitter-input frame-ending
markers preceded the native withdrawal by approximately 503 and 542 ms;
last decoded observations were similarly old, and no markers were dropped by
the bounded diagnostic. These probes bound this interruption to after sender
payloader output and before receiver jitter-input marker observation, but do
not identify a network segment, prove all packets missing, or measure actual
monitor scan-out. Evidence: `ui-compact-40-6a8-6193`.

Attempt 41 stopped after 241.75 seconds on a critical-video wait; all 40 input
windows matched, including four with nonzero brake. All 241 sampled logical
paths were DC; WSS delivered 213 accepted copies first. At the wait, vehicle
RTP payloader-output maximum gaps were 55 and 42 ms, with latest events 4 and
8 ms old. The two receiver jitter-input frame-ending markers were last seen
approximately 523 ms before withdrawal, with no diagnostic marker drops. The
accepted control-result RTT p99 was 62 ms and maximum 86 ms, so this is not
proof that control packets or the entire Wi-Fi link stopped during the same
interval. Evidence: `ui-compact-41-6a8-6193`.

Attempt 42, using Windows `6a8fd48` and Orin `6193b9b`, completed 1,800.88
seconds with no recorded client safety-stop episode or vehicle braking wait.
All 300 scheduled input windows matched; the nine scenarios were represented
and all 33 brake windows contained a nonzero request. All 1,791 sampled
logical paths were DC and the selected pair was direct, but 1,660 accepted
copies arrived over WSS first and 33,111 over DC first. Accepted-result RTT
p50/p95/p99/max was 50/62/72/97 ms; excluded rejected or missing results
are not represented in that distribution. Maximum sampled time uncertainty
was 16 ms; CAN was DOWN. The Orin process required forced termination during
post-window teardown, so the ended process code was -9; this was after the
measurement and is not evidence of graceful shutdown. Values are zero-limited
mock commands, not real-vehicle actuation. Earlier attempts failed, so this
one successful window does not establish repeatable stability. Evidence:
`ui-compact-42-6a8-6193`.

Attempt 43 used Windows `d07ea8e` and stopped after 174.72 seconds on a
critical-video wait. Of 29 input windows, 28 matched; the final reverse window
was interrupted, and three brake windows contained nonzero requests. All 174
sampled logical paths were DC; WSS delivered 64 accepted copies first. Both
last ICE-source RTP frame-ending markers and jitter-input markers preceded the
client withdrawal by approximately 559 ms; neither bounded probe dropped
markers. At the vehicle wait, RTP payloader-output maximum gaps were 48 and
42 ms, with latest events 30 and 32 ms old. Accepted-result RTT p99 was 67 ms
and max 80 ms. This places the observed video marker gap before the ICE
source callback and after the sender payloader probe; it does not distinguish
sender WebRTC buffering, the network, or receiver socket delivery. It does
not establish a complete absence of network packets. Evidence:
`ui-compact-43-d07-6193`.

Attempt 44 stopped after 66.36 seconds on a control-link wait; all 11 input
windows matched, including one nonzero brake window. All 66 sampled logical
paths were DC; WSS delivered 27 accepted copies first. Accepted-result RTT
p99 was 57 ms and max 417 ms. The two receiver matched-marker frame-gap
maxima were approximately 514 and 524 ms, although the recorded braking
cause was control link. These observations do not establish that a particular
video packet or command caused the wait. Evidence: `ui-compact-44-d07-6193`.

Attempt 45 stopped after 480.12 seconds on a control-link wait. Of 80 input
windows, 79 matched; the final reverse-right window was interrupted, and
eight brake windows contained nonzero requests. Logical control used DC in
473 samples and WSS in five; the media pair remained direct. WSS delivered
234 accepted copies first and DC 9,004. Accepted-result RTT p99 was 58 ms
and max 91 ms; missing or rejected results are excluded. Evidence:
`ui-compact-45-d07-6193`.

Attempt 46 used Windows `d07ea8e` and Orin `6ac2236` and stopped after 66.64
seconds on a critical-video wait. All 11 input windows matched, including one
nonzero brake window; all 67 sampled logical paths were DC and WSS delivered
34 accepted copies first. The sender ICE-sink probe observed both RTP sources
with maximum marker gaps of 46 and 54 ms and last-marker ages of 9 and 11 ms
at the vehicle wait. At the client withdrawal, the corresponding receiver
ICE-source markers were approximately 557 and 551 ms old, with no diagnostic
source or marker drops. The client withdrawal and vehicle wait wall times
were about 20 ms apart, subject to host clock uncertainty. This bounds the
observed frame-ending-marker interruption to after sender ICE-sink entry and
before receiver ICE-source observation. It does not prove socket delivery,
that all RTP packets were lost, or which OS stack or network segment delayed
them. Evidence: `ui-compact-46-d07-6ac`.

Attempt 47 used Windows `d07ea8e` and Orin `6ac2236`; it stopped after
1,500.15 seconds on a control-link wait. Of 250 input windows, 249 matched;
the final reverse-left window was interrupted, and 27 brake windows contained
nonzero requests. The selected media pair remained direct; logical control
was DC in 1,487 samples and WSS in five. Accepted copies arrived first over
DC 28,273 times and WSS 835 times. Accepted-result RTT p99 was 57 ms and max
86 ms, excluding missing or rejected results. This window failed the
30-minute uninterrupted-control acceptance. Evidence:
`ui-compact-47-d07-6ac`.

Attempt 48 used Windows and Orin `6ac2236`; it stopped after 187.94 seconds on
a critical-video wait. All 31 input windows matched, including three nonzero
brake windows; all 187 sampled logical paths were DC, and WSS delivered 86
accepted copies first. Sender ICE-sink marker maximum gaps were 54 and 44 ms,
while the corresponding last receiver ICE-source markers preceded withdrawal
by approximately 559 ms. This repeats the attempt-46 boundary, but does not
identify the responsible network segment or prove kernel packet delivery.
Post-stop frame-gap maxima include pipeline teardown and are not presented as
measurement-window latency. Evidence: `ui-compact-48-6ac-6ac`.

Attempt 49 used Windows and Orin `6ac2236` and stopped after 421.89 seconds on
a critical-video wait. All 70 input windows matched, including seven nonzero
brake windows; all 420 sampled logical paths were DC and WSS delivered 190
accepted copies first. Sender ICE-sink marker maximum gaps were 52 and 48 ms;
receiver ICE-source markers were last observed approximately 504 and 551 ms
before withdrawal. No diagnostic source or marker drops were recorded. This
again constrains the observation to the interval between those probes, not a
specific network segment or kernel packet-delivery result. Evidence:
`ui-compact-49-6ac-6ac`.

Attempt 50 used Windows and Orin `6ac2236`, stopping after 205.12 seconds on
a control-link wait. All 34 input windows matched, including three nonzero
brake windows; all 204 sampled logical paths were DC, while WSS delivered 100
accepted copies first. The Windows gateway probe timed out at 400 ms near
the wait and adjacent samples were 2--4 ms. The two recorded receiver frame
marker gaps reached approximately 580 ms. Of 273 sender DTLS records in the
surrounding window, 266 matched unambiguously at the vehicle kernel; eight
approximate corrected delays exceeded 100 ms, with a maximum of 332 ms. The
records are encrypted and cannot be assigned to specific control commands;
the correlation does not identify the entire cause. Evidence:
`ui-compact-50-6ac-6ac`.

Attempt 51 used Windows and Orin `6ac2236`; it stopped after 54.15 seconds on
a critical-video wait. All nine input windows matched, including one nonzero
brake window; all 54 sampled logical paths were DC and WSS delivered 15
accepted copies first. Sender ICE-sink marker maximum gaps were 44 and 45 ms;
receiver ICE-source markers were last observed approximately 556 ms before
withdrawal. Neither bounded trace reported drops. Maximum post-stop frame-gap
statistics include teardown and are not treated as measurement latency.
Evidence: `ui-compact-51-6ac-6ac`.

Attempt 52 used Windows and Orin `6ac2236`; it stopped after 90.62 seconds on
a critical-video wait. Of 15 input windows, 14 matched; the final forward-right
window was interrupted, and one brake window contained a nonzero request.
All 90 sampled logical paths were DC; WSS delivered 56 accepted copies first.
Post-stop frame-gap maxima include teardown. Evidence: `ui-compact-52-6ac-6ac`.

Attempt 53 used Windows and Orin `6ac2236` and stopped after 96.60 seconds on
a control-link wait. All 16 input windows matched, including one nonzero
brake window. Logical control was DC in 94 samples and WSS in two, while the
selected media pair remained direct; WSS delivered 54 accepted copies first.
The application DTLS trace reported one drop, so no packet-loss conclusion is
drawn from that trace. Evidence: `ui-compact-53-6ac-6ac`.

Attempt 54 used Windows and Orin `6ac2236` and stopped after 7.11 seconds on
a critical-video wait. The only scheduled input window matched; all eight
sampled logical paths were DC and WSS delivered five accepted copies first.
The optional Linux AF_PACKET SOCK_DGRAM capture was filtered to outbound RTP
frame-ending headers between the two test IPv6 addresses and recorded metadata
only, never payload. Around the vehicle wait (approximately +/- 1 second), it
observed 60 markers per stream, with maximum gaps 39.8 and 37.4 ms. The
corresponding Windows ICE-source markers were approximately 516 ms old at
withdrawal. The packet socket and bounded trace reported no drops. AF_PACKET
outgoing observation precedes confirmed NIC transmission; this narrows the
interval to after that tap and before the Windows ICE source, and does not
prove packet delivery or isolate either host or any network segment. Evidence:
`ui-compact-54-6ac-6ac`.

Attempt 55 used Windows and Orin `6ac2236` and stopped after 1,585.31 seconds
on a control-link wait. All 264 input windows matched, including 29 nonzero
brake windows. Logical control was DC in 1,574 samples and WSS in three; the
selected media pair remained direct. WSS delivered 792 accepted copies first
and DC 30,014. Accepted-result RTT p99 was 60 ms and max 363 ms, excluding
missing or rejected results. Around the wait the Windows gateway probe rose
to 222 ms and then timed out at 400 ms; adjacent probes were 2--3 ms. Of 377
unambiguously matched encrypted DTLS records in the surrounding window, 23
had approximate corrected delays over 100 ms and the maximum was 350 ms.
The RTP kernel metadata capture reported zero socket or limit drops. These
observations correlate but cannot attribute individual encrypted control
commands or identify the sole faulty segment. Evidence:
`ui-compact-55-6ac-6ac`.

Attempt 56 used Windows and Orin `6ac2236`; it stopped after 1,044.35 seconds
on a control-link wait. All 174 input windows matched, including 19 nonzero
brake windows. The complete logs show multiple control-link braking waits
and two client safety-stop episodes. The selected media pair remained direct;
logical control was DC in 1,036 observations and WSS in three. Accepted
copies arrived first via DC 19,873 times and WSS 448 times. Accepted-result
RTT p99 was 62 ms and max 500 ms, excluding missing or rejected results.
Evidence: `ui-compact-56-6ac-6ac`.
