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

The separate Windows and Orin startup clock probes reported uncertainties of
14 ms and 19 ms respectively; these are individual preflight samples, not a
guarantee throughout a session. No physical gamepad was connected. Raw logs
and analysis remain in the local artifact directory under `ui-preflight-01-87`.
No 30-minute result, physical CAN result, or real-vehicle acceptance is
established by this preflight.
