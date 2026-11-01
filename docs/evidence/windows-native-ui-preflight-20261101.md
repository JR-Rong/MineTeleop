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
