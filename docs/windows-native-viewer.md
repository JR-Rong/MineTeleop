# Windows native bench viewer

`mine-teleop-windows-viewer.exe` is an experimental native Win32 and GStreamer
H.264 viewer. Its default mode does not use a web page, accept driving input,
or send control commands. The in-process receive-only runtime disables the native
control sender, control WSS, and control APIs. It is not a driving console and
does not prove safety readiness, display scan-out, direct connectivity, or
latency. No production or real-vehicle use is supported.

An experimental, explicit `--mock-control --local-ip <ip> --peer-ip <ip>`
mode exercises limited keyboard input against an isolated `test_bench_` mock
vehicle with zero speed, two critical cameras, a loopback signaling tunnel and
the existing 25 ms clock limit. Both addresses must be distinct public addresses
in the same family. The receiver filters candidates to those endpoints and the
SCTP transport must independently select the same UDP pair. Local configuration
is not proof of remote containment: the test harness must verify the remote
process, mock adapter, zero-speed configuration, CAN-down state and cellular
route before starting. The profile retains neutral gear, zero speed, zero motor
torque and zero steering limit; this mode is not a motion-control interface.
Focus the main viewer or click a video panel. W/up and S/down request forward
and reverse respectively, A/left and D/right request mock steering, space
requests service brake, and B requests hard brake. A gear change is sent with
zero traction and steering; the held direction is applied only after vehicle
telemetry confirms the requested gear at zero speed. Confirmation must be fresh
both by local receipt time and by the vehicle's source timestamp, accounting
for both endpoints' existing clock uncertainty limits. Both direction
keys together request no traction, and braking takes precedence. Brake ratios come from the acknowledged session
profile; hard brake takes precedence. E or the mock emergency button latches
an emergency request. The display distinguishes a request from fresh vehicle
telemetry confirming the software latch; it does not prove physical execution.
Braking wins over throttle. Input is neutralized when focus, the UI heartbeat,
critical video, clock, direct media path, acknowledged profile, fresh mock
telemetry or control readiness is lost. Held keys must be released before a
new press can take effect after a loss. There is no production emergency-stop
input or authorization in this prototype.

The native status strip follows the web console's distinction between measured
speed and gear and locally requested gear, steering, throttle and brake.
Measured values require fresh vehicle telemetry, including its source time;
requested values require a fresh accepted local input and active authorization.
Unknown or stale values display as `—`. Keyboard highlights describe local key
state, not vehicle receipt or physical actuation, and clear on focus loss.

After login the viewer presents the authorized vehicle catalog and requires an
explicit selection. This bench prototype can connect only an online,
controllable vehicle whose identity matches its validated local vehicle
configuration. The server still independently authorizes the connection.

Mock input supports standard XInput controllers in addition to the keyboard.
It applies the configured standard-controller calibration and deadzone, and
requires neutral pedals after focus, safety, profile or controller changes.
Generic HID devices, interactive calibration and gamepad controls outside
mock mode are not implemented. A scrollable CAN panel shows valid, fresh
feedback fields and reports missing, unsupported or stale feedback rather than
treating mock data as physical CAN. Interacting with this panel removes input
focus and the existing control interlock requires a fresh release afterwards.

The login/vehicle-selection screen also offers an export for authorized,
completed session logs through the existing driver runtime. It must run while
there is no active session, and its ZIP does not contain video. Partial export
or unconfirmed recording completeness must not be interpreted as a complete
recording. An isolated mock preflight listed authorized sessions and saved a
readable ZIP at the default location; all three log sources in that ZIP were
partial. Automated verification of a user-selected save path remains open.

The native viewer is not yet functionally equivalent to the web console. It
does not implement all of the web console's CAN presentation, editable session
profile or recording status. Its control mode remains
limited to the zero-speed isolated mock. Keyboard and standard-gamepad control
values use the acknowledged profile scaling, so that profile sends zero
throttle and steering in this mock configuration. Directional key presses
must therefore be verified separately from delivered motion values in a bench
test; a zero value alone is not proof that a key was or was not pressed.
Mock gear selection currently waits for fresh matching telemetry, but does not
implement the web client's complete status-sequence-fenced transition and
correlated rejection/rollback logic. It must not be treated as equivalent
production gear control.

For the `b93bfff` UI build, a Windows smoke check opened the isolated mock
window without connecting a vehicle, checked four edit fields and Tab focus,
and inspected 1100×720 and 900×720 window captures. One minimum-size capture
was blank; a repeat displayed the expected layout, so a transient capture or
paint issue is not ruled out. The earlier 30-minute mock result used a different
viewer build and does not validate this UI change under live video load.

An additional dual-control experiment requires the vehicle process to set
`MINE_TELEOP_MOCK_DUAL_CONTROL=1`; the vehicle advertises it only for a
`test_bench_` identity, mock adapter and zero speed. The mock native viewer
then sends the same authenticated, sequenced command via its DC and the cloud
WSS path. The receiver admits the first fresh copy and rejects duplicates and
older commands without reapplying them or refreshing safety; an ingress ACK is
not vehicle receipt. While the cloud authority is live, the same bounded
vehicle telemetry snapshot is also offered over WSS; duplicate or delayed
snapshots cannot advance the viewer's telemetry sequence or source time. A
selected direct media path and all existing authority,
video, input and clock gates remain required. The paths may share the same
Wi-Fi and cellular links and cannot guarantee independence or bounded delay.
This is an isolated experiment, not a production transport or safety claim.

With `logging.control_trace_commands` enabled, mock mode batches a bounded
record of command sequence, local DataChannel enqueue start/end and outcome.
The record contains no raw packet or credentials; local enqueue is not proof of
delivery. Correlate it with the vehicle receipt and driver dispatch traces to
locate delay, accounting for clock uncertainty between hosts.
Mock-only input trace batches also record the locally armed key mask and the
intent sequence. Different key states receive different intent revisions even
when zero limits produce identical commands; correlate those revisions with
vehicle `receive_apply` records. The key mask is not sent to the vehicle.
The render summary separates decoded-sample callback intervals, foreground
submission intervals, and callback-to-submission time. These are local
measurements; they do not establish capture-to-display or physical scan-out
latency. Samples are bounded and an omitted count is reported.
It also emits bounded transition records for focus, UI heartbeat, clock,
video, remote media, direct path and input authorization. A null check means
that an earlier guard short-circuited it; dropped records are counted. These
records distinguish the reasons for withdrawn input, but do not prove why a
frame or command was delayed.

The two video areas belong to the Win32 application. Signaling and frame
reception reuse the native driver runtime and the existing receiver probe;
the latter is a bounded measurement implementation, not a finished production
renderer. The session stops on renegotiation or media error and must be
reconnected manually. Restart before the receiver's 131072-PTS observation
limit is reached. A drawn frame may remain displayed after a stall; it is not
evidence of a fresh camera frame. The status bar reports whether new GDI frame
submissions advanced for every configured track in the last sample and whether the selected
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
The optional driver ID field overrides the ID from the driver configuration;
an empty field retains the configured ID. The existing runtime validates the
identity and authenticates it with the supplied password.
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
are connected to the isolated mock mode. They reject a
mismatched negotiation, unsuitable channel properties,
late messages after retirement, duplicate control channels, oversized input
and queue overflow. GStreamer without the early channel callback is rejected;
local send success means queued, not vehicle receipt. Receive-only mode remains
enforced outside the explicitly scoped mock mode.
The native publication filter rejects expired, replayed, wrongly scoped or
byte-mismatched bridge frames before a one-time send attempt. The caller
serializes sends with session and owner retirement.
An isolated integration fixture exercises that filter with the real control
bridge, signed commands and a mock vehicle service; it simulates the DC byte
path and does not prove WebRTC delivery or physical vehicle behavior.
With GStreamer 1.28 or later the experimental channel can inspect the selected
ICE pair of the SCTP transport itself and classify a connected non-relay UDP
pair. Older versions and missing or ambiguous state return false. Mock mode
also checks the expected addresses; the snapshot alone is not WAN proof or
evidence of a TURN leg's protocol.

The isolated mock mode is wired to `update_control_intent` with session and
input-ownership fences, an input lease, focus handling, profile acknowledgement,
time-sync readiness and vehicle feedback. A production driving client still
needs its own complete input-device, emergency-stop and safety validation. It
must derive each critical camera's freshness from real
frame identity and timing, with bad-sample and brake revisions; decoded or drawn
frame counts alone are insufficient to rearm control. Retained or repeated
images must never refresh that evidence.

The receiver drops decoded buffers with invalid, corrupt, duplicate or
regressing PTS. A timestamp discontinuity requires a new track or session;
this conservative filter is only a prerequisite and does not prove that a
fresh camera image was displayed.

An experimental `NativeVideoHealth` state machine tracks each critical
camera's generation, submitted frame identity, expiry and brake revisions. The
window feeds it after successful foreground GDI submission of a MID-mapped,
validated decoded frame; focus loss or minimization withdraws usability. The
renderer keeps only the newest frame per camera. In mock mode the control gate
uses this signal together with separate vehicle media evidence. This is not
display scan-out or proof of vehicle capture freshness. Its tests also
exercise two synthetic streams through the driver bridge and mock vehicle
service; that fixture does not validate the Windows renderer or the vehicle.

The viewing prototype binds each video section's negotiated MID and ordered
`media_tracks` identity to the configured camera window, rejecting missing or
ambiguous identities and duplicate bindings. Renegotiation still requires a
new viewer session. A control-capable client must also validate this mapping
across the actual vehicle, track replacement and recovery paths; window position
or track count alone cannot establish which critical camera is fresh.
The synthetic two-stream loopback additionally compares each mapped camera to
the RTP SSRC assigned to its known test source and checks that deliberately
swapped `media_tracks` fail that comparison. This is a fixture assertion, not
runtime authentication of a physical camera or proof of displayed freshness.

The mock mode uses the scoped native WebRTC DataChannel courier, frame expiry,
result and feedback handling, and WSS fallback. Validation must use an isolated
mock vehicle, zero-speed limits and CAN down, and separately test stalls,
disconnect, stale frames, input loss and reconnection before any long-run
comparison. The existing 25 ms commissioning threshold is not changed here.

The native-publication safety fixture injects reported critical-video failure
and an expired input lease into the real driver bridge and mock control service,
and checks that each reaches the corresponding braking wait. The byte courier
and video health are simulated, and the initial recovery wait is explicitly
released by the fixture: this does not test real WebRTC, displayed freshness,
the vehicle media recovery coordinator, or automatic recovery.
