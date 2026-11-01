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
