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
