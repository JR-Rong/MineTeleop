# Windows native bench viewer

`mine-teleop-windows-viewer.exe` is an experimental native Win32 and GStreamer
H.264 viewer. It does not use a web page, accept driving input, send control
intents, or establish a control DataChannel. It is not a driving console and
does not prove safety readiness, display scan-out, direct connectivity, or
latency. No production or real-vehicle use is supported.

The two video areas belong to the Win32 application. Signaling and frame
reception reuse the native driver runtime and the existing receiver probe;
the latter is a bounded measurement implementation, not a finished production
renderer. The session stops on renegotiation or media error and must be
reconnected manually. Restart before the receiver's 131072-PTS observation
limit is reached. A drawn frame may remain displayed after a stall; it is not
evidence of a fresh camera frame.

## Building and starting

The Windows CI workflow builds `mine-teleop-windows-viewer` alongside the
existing probe with the pinned GStreamer SDK. The workflow artifact contains
the two executables, not a self-contained installer. Install the matching
GStreamer runtime separately and make its `bin` directory available on PATH;
ensure the WebRTC, libnice, H.264 decoder and GL plugins are available.
Start the viewer from a working directory containing the chosen configuration
files, or enter absolute paths in its two configuration fields. Enter the driver
password in the password field and choose Connect. Disconnect closes the
session; closing the window also requests disconnection.

The vehicle YAML supplied to the viewer must describe one or two H.264 cameras,
media enabled, control disabled, mock adapter, bench mode, zero speed, and
disabled recording and upload. The driver YAML supplies the authorized driver
identity and signaling URL. Public signaling requires HTTPS/WSS under the
existing runtime policy. The viewer aligns its time-sync settings with the
supplied vehicle config and does not relax the safety threshold.

**The local YAML is not an attestation of the actual vehicle process.** Before
connecting, independently verify that the running vehicle uses an equivalent
isolated mock configuration, CAN is down, and the intended cameras are not in
use by another task. The viewer can create a signaling session but never sends
control commands. Do not operate a real vehicle with this experiment.

The executable's `--validate-config FILE` checks the local admission rules and
exits without starting a window or network session; the Windows CI exercises
both accepted and rejected configurations. Actual Windows/Orin video, control,
long-duration stability, and browser/native comparison require separate bench
evidence and are not established by that test.
