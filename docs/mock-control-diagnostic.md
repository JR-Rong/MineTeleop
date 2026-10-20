# Mock control diagnostic

This opt-in mode measures the existing control transport and mock application
feedback without opening cameras or a CAN adapter. It does not validate video,
physical braking, or real-vehicle operation.

Use matching newly built vehicle and driver packages. In an isolated vehicle
configuration, set `runtime.mock_control_diagnostic: true`, keep control and
media runtime enabled, use a `test_bench_` vehicle identity and the built-in
`mock` adapter, set `cameras: []`, and disable recording and upload. The media
runtime still owns signaling, WebRTC DataChannel and native WSS control.
Existing unused media profiles may remain in the configuration.

In the isolated driver configuration set `control.mock_control_diagnostic:
true`. Ordinary configurations default to false. The driver refuses other
vehicle identities. The browser requires fresh status from the matching mock
session before allowing input without video. Commands carry the mode flag;
the vehicle rejects a mode mismatch. Authenticated ESTOP remains available.

Camera health remains unassessed and false. Only the video prerequisite and
video recovery samples are omitted in this mode. Authentication, session
profile acknowledgement, sequence/freshness checks, clock trust, input lease,
ACK recovery, neutral rearm, watchdog, limits and stop handling remain active.

Run diagnostics with independent owner deadlines and stop/archival supervision.
Record sent, received, rejected, applied and acknowledged commands separately.
Record the actual native control path and selected ICE pair: an open
DataChannel can still use TURN, and a connected peer does not prove commands
were sent over that channel. A successful control-only window is not a video
or combined-load acceptance result.
