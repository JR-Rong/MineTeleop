# Windows native bench viewer

`mine-teleop-windows-viewer.exe` is an experimental native Win32 and GStreamer
H.264 viewer. It does not use a web page, accept driving input, or establish a
control DataChannel. The in-process receive-only runtime disables the native
control sender, control WSS, and control APIs. It is not a driving console and
does not prove safety readiness, display scan-out, direct connectivity, or
latency. No production or real-vehicle use is supported.

The two video areas belong to the Win32 application. Signaling and frame
reception reuse the native driver runtime and the existing receiver probe;
the latter is a bounded measurement implementation, not a finished production
renderer. The session stops on renegotiation or media error and must be
reconnected manually. Restart before the receiver's 131072-PTS observation
limit is reached. A drawn frame may remain displayed after a stall; it is not
evidence of a fresh camera frame. The status bar reports whether draw callbacks
advanced for every configured track in the last sample and whether the selected
candidate pair is direct UDP, relay, other, or unknown. A separate window timer
warns if the worker stops updating the status. These indicators do not prove
camera capture freshness, monitor scan-out, or control safety.

## Building and starting

The Windows CI workflow builds `mine-teleop-windows-viewer` alongside the
existing probe with the pinned GStreamer SDK. The workflow artifact contains
the two executables, not a self-contained installer. Install the matching
GStreamer runtime separately and make its `bin` directory available on PATH;
ensure the WebRTC, libnice, H.264 decoder and GL plugins are available.
From the artifact directory, run `./run-native-viewer.ps1 -GStreamerRoot
<runtime-directory>`. The launcher checks the required elements, scopes the
runtime environment to the process, and starts the viewer. Use `-CheckOnly` to
check dependencies without opening a window. Enter absolute paths in the two
configuration fields, enter the driver password, and choose Connect.
Disconnect closes the session; closing the window also requests disconnection.

The vehicle YAML supplied to the viewer must describe one or two H.264 cameras,
media enabled, control disabled, mock adapter, bench mode, zero speed, and
disabled recording and upload. The driver YAML supplies the authorized driver
identity and signaling URL. Public signaling requires HTTPS/WSS under the
existing runtime policy. The viewer aligns its time-sync settings with the
supplied vehicle config and does not relax the safety threshold.

**The local YAML is not an attestation of the actual vehicle process.** Before
connecting, independently verify that the running vehicle uses an equivalent
isolated mock configuration, CAN is down, and the intended cameras are not in
use by another task. The viewer still creates the existing exclusive session
and renews its session authority; this is not a passive multi-viewer session.
Do not operate a real vehicle with this experiment.

Historical correction: builds through `a96ca73` did not set receive-only mode.
Although they had no driving input, their reused runtime bootstrapped a neutral
intent and could attempt control heartbeats via WSS. Prior statements that
those builds could not send any control command were incorrect. Those tests ran
with vehicle control disabled, a mock adapter, zero speed, and CAN down.

The executable's `--validate-config FILE` checks the local admission rules and
exits without starting a window or network session; the Windows CI exercises
both accepted and rejected configurations. Actual Windows/Orin video, control,
long-duration stability, and browser/native comparison require separate bench
evidence and are not established by that test.

## Control work still required

The experimental scoped DataChannel courier and its bounded receive mailbox
are available as an explicit receiver option, but are not enabled in this
window or connected to the production transport bridge. They reject a
mismatched negotiation, unsuitable channel properties,
late messages after retirement, duplicate control channels, oversized input
and queue overflow. GStreamer without the early channel callback is rejected;
local send success means queued, not vehicle receipt. The loopback test does
not establish path identity, command authentication, video freshness, or any
vehicle-control behavior. Receive-only mode remains enforced in the window.

The native window is deliberately not wired to `update_control_intent`. A
driving client must first connect the existing session-generation and input
ownership fences, neutral takeover, input lease, focus/device-loss handling,
emergency-stop latch, control-profile acknowledgement, time-sync readiness and
vehicle feedback. It must derive each critical camera's freshness from real
frame identity and timing, with bad-sample and brake revisions; decoded or drawn
frame counts alone are insufficient to rearm control. Retained or repeated
images must never refresh that evidence.

The receiver drops decoded buffers with invalid, corrupt, duplicate or
regressing PTS. A timestamp discontinuity requires a new track or session;
this conservative filter is only a prerequisite and does not prove that a
fresh camera image was displayed.

The viewing prototype binds each video section's negotiated MID and ordered
`media_tracks` identity to the configured camera window, rejecting missing or
ambiguous identities and duplicate bindings. Renegotiation still requires a
new viewer session. A control-capable client must also validate this mapping
across the actual vehicle, track replacement and recovery paths; window position
or track count alone cannot establish which critical camera is fresh.

For direct control, the native WebRTC DataChannel must also implement the
existing scoped transport bridge, frame expiry, result and feedback handling,
and its WSS fallback without replaying an old command across a change of
session, owner, negotiation or route. Initial validation must use an isolated
mock vehicle, zero-speed limits and CAN down, and separately test stalls,
disconnect, stale frames, input loss and reconnection before any long-run
comparison. The existing 25 ms commissioning threshold is not changed here.
