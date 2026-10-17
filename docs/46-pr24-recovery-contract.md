# PR24 recovery and exit contract

This replaces the readiness deadline descriptions in the historical session
reports. Those reports remain evidence of their own source versions, not the
current contract. No threshold was widened to make a test pass.

## Independent samples and recovery

Video, browser input refresh, advancing ACKs, and time-sync cohorts have separate
consecutive anomaly counters. A new valid normal sample clears only its category.
The third consecutive bad sample withdraws control readiness. Repeated status
polls, duplicate or reordered data do not count or clear a counter. Missing new
data has an independent timeout. ESTOP and explicit chassis faults act without
three-sample tolerance.

Samples are actual observations: a new owned HTTP refresh arrival, an ACK for
an actually sent advancing sequence, a completed or failed time-sync cohort, or
a new video picture/state event. Native resampling of an old browser intent is
not another browser refresh. Vehicle video uses capture PTS age at the send
queue source, not encoder callback arrival time or a network delivery claim.

Each producer preserves bad-event and brake-event revisions after normal data
returns. This prevents latest-value merging from hiding a short fault or a
third-sample threshold crossing. The receiver resets the common health window
on changed bad history and starts braking wait on changed brake history.
Repeated unchanged history does not restart that window.

While waiting, the authenticated session and profile remain installed and
independent braking remains active. All required conditions must remain healthy
for 300ms; then the operator must release old controls and press again. Neutral
heartbeats during the wait cannot rearm an old held input. Elapsed readiness
waiting alone does not hard-lock the session at five seconds. Genuine command,
feedback and ACK-progress absence, transport failure, authority expiry, ESTOP
and chassis faults retain their independent protection.

`control_recovery_timeout_ms` remains in profile version 7 for bounded transport
recovery and historical diagnostics. It is no longer a wall-clock deadline
which revokes a session solely because video, input or clock readiness remains
unavailable. The common latch reports a null readiness deadline.

Only enabled cameras marked `critical_for_control` participate in video
interlocking. All false is supported; noncritical video cannot clear a critical
fault, and other control protections remain required. Startup grants no motion
authority before valid admission frames.

## Clock, input ownership and protocol

The 25ms clock uncertainty default and 200ms native/browser freshness budgets
are retained. Clock cohorts check full-cohort continuity, paired wall/steady
read error and server UTC interval consistency. Rejected cohorts preserve the
previous accepted steady-based mapping. Capture uses monotonic acquisition
timestamps when the device explicitly supplies them. Clock scheduling/anchor
absence budgets do not prove physical oscillator drift bounds.

A new page confirms a neutral takeover at the expected ownership epoch. Old
pages cannot refresh ordinary motion or reclaim ownership with late neutral
requests. Legal authenticated ESTOP retains priority. Browser abort is not a
claim that the server did not process the request.

The current native capability is `v9-revision-recovery-v1`. Envelope version 1
and profile version 7 remain distinct. Cloud and the selected registered vehicle
must both declare the capability before issuing native control authority;
unknown, V7 and the intermediate V8 candidate are rejected. Capability changes
rotate the vehicle connection generation. The currently deployed older cloud
cannot be treated as compatible merely because it supported the former V7
candidate. Update planning must account for all participating binaries.

## Video freshness and capture rate

The send branch drops an overaged encoded AU and fences dependent pictures
until a fresh true IDR carries the required parameter sets. H.265 CRA and a
non-delta flag alone do not prove a safe restart. A key-unit request is sent
upstream. The recording branch retains its reference chain.

CCG2 capture requests 20FPS and reports S_PARM and G_PARM separately. Production
raw videorate is drop-only and caps encoder input even if a driver declares 20
but delivers 30. That is a fallback, not proof that sensor acquisition is 20.
Measure capture delivery, encoder input, encoded send and decoded output over
one window. The explicit bench probe is opt-in and never an automatic CTest;
it allows only channels 6/7, a dedicated configuration, control disabled and a
mock adapter. It does not create CAN, HTTP or WebRTC runtimes.

## Exit and operator feedback

VCU reverse handshake requires new safe feedback in the relevant phases; bridge
Disarmed remains masked until the final safe CAN socket write succeeds. Close
timeout/write failure returns failure, never a completed exit. CAN socket success
and protocol feedback do not prove mechanical parking.

The page freezes ordinary input, requests VCU disconnect and retains its peer
for bounded confirmation. A new same-session status with trustworthy source
time, Disarmed and complete fresh N/zero-speed/manual/EPB feedback can confirm
the protocol. Timeout, lost feedback, unsupported adapters, old status and
release failure explicitly report unconfirmed exit and the next operator action.
The 15s chassis budget is unchanged. HTTP logout success is not exit evidence.

HTTP disconnect/end-session atomically validate expected identity, session,
generation, page owner and ownership epoch. Pending approval cancellation also
validates the initiating page and connect generation. A release reservation
blocks a new takeover during slow cloud I/O; legal ESTOP remains available and
a failed release clears the reservation for retry.

The existing VCU DataChannel command has no page epoch. A takeover racing between
the page's owner check and its VCU-disconnect send can still request disarm on an
old current channel; the guarded HTTP release cannot release the replacement's
scope. This is a remaining safe-stop availability boundary, not a motion grant.

## Validation boundary

Portable CTest, real loopback HTTP/WebSocket faults, production producer/latch
tests and synthetic desktop lifecycle tests are separate from Linux media,
packaged release, physical camera and vehicle acceptance. The offline CI runner
requires loopback-only networking for full native tests and clears external-test
overrides. Its additional Ubuntu/GStreamer coverage does not replace release
platform checks. Do not report unexecuted or skipped hardware tests as passes.
