# Orin NX hardware encoder and mock control, 2026-11-17

Status: Jetson hardware encoding is operational. A18 and A19 failed. Windows
GUI testing is unavailable because the user cannot log into its desktop. A20
used Windows headless Edge over SSH and also failed. All three use the
authorized shared LAN; none establishes physical display, WAN/5G or vehicle
acceptance.

## Target and minimal adaptation

The updated target is accessed through SSH alias `nx`: aarch64, HCK-C1202
Orin NX/Nano device-tree model, compatible p3767-0000, L4T R36.5.0. The actual
`/dev/v4l2-nvenc` device and successful encoding establish usable hardware;
factory presence alone was not treated as proof. The original 40W power mode
was read, not changed. CAN remained down and both real UYVY cameras were free
before starting the isolated mock fixture.

PR26's existing `nvenc` backend selected only desktop NVCodec factories.
It now also discovers Jetson `nvv4l2h264enc` / `nvv4l2h265enc`, after the existing
desktop choices. The Jetson-specific stage inserts `nvvidconv` and NVMM/NV12
caps, converts kbit/s to bit/s with an overflow check, and uses its own CBR,
I/IDR intervals, no-B-frame, parameter-set and timestamp-copy properties.
Desktop preset/tune handling is unchanged. No new backend enum or application
queue was introduced, and no safety deadline was relaxed.

`configs/vehicle-agent.orin-nx-isx031-bench.yaml` requires hardware with no CPU
fallback and retains mock/zero-speed/recording-off/upload-off isolation. Its
example bitrate is 2500 kbit/s; the private A18/A19 fixtures use the existing
comparison bitrate of 1500 kbit/s per 960x768/30fps stream, captured at
1920x1536/30fps UYVY. Capture, CPU color conversion and scaling still precede
NVMM upload; this is not an end-to-end zero-copy claim.

NVIDIA's [accelerated GStreamer documentation](https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Multimedia/AcceleratedGstreamer.html)
was cross-checked against the actual installed plugin properties. No global
JetPack package upgrade was performed: missing Ubuntu WebRTC/nice/parser
plugins were extracted into a private runtime directory. Production codec and
mock tests were built in an isolated ARM64 Ubuntu container.

## Functional proof

- Core C++ tests: 90/90, including unchanged desktop NVENC properties, Jetson
  bitrate/memory contracts and the hardware-only mock configuration.
- Codec wrapper tests: 4/4. An initial discovery invocation found zero tests;
  it is retained and is not counted as a pass.
- Synthetic H.264 and H.265: actual selected Jetson factory, first IDR, hardware
  NVDEC output and exact input/decoded PTS identity all passed. Each encoder
  correlated 16 inputs/parsed outputs. These are bounded startup tests, not
  sustained display or network performance results.
- The vendor library prints `Opening in BLOCKING MODE` to stdout. The strict
  JSON wrapper initially rejected that output despite worker success. The
  opt-in C++ worker now preserves a separate report descriptor and routes vendor
  stdout to stderr throughout initialization/teardown. JSON parsing was not
  weakened; raw failed receipts were retained.
- Real-camera single-stream check encoded 60 frames. Both production media lanes
  subsequently logged actual `encoder_factory=nvv4l2h264enc`.
- A separate two-real-camera functional probe correlated 180 frames per camera,
  first IDR, maximum seven-frame IDR spacing, and fresh IDR output after a
  standard upstream force-key-unit request. It uses a standalone GStreamer/Python
  capture observer and achieved only about 12.6fps on one lane; it is not used
  as production throughput or CPU/hardware A/B evidence. The actual application
  runtime reported approximately 29.9fps per lane near A18's incident.

H.265 readback confirmed bitrate 1500000, constant-bitrate control, I/IDR
intervals 7, B frames 0, SPS/PPS insertion and timestamp copy. Default virtual
VBV size was 4000000 bits; it was not silently reduced or equated to measured
queue delay. Hardware pool capacity/zero-copy/render-surface budgets remain
unverified. The receiver snapshot had 10797 decoded/1543 key frames per lane,
zero decoded drops and zero reported freezes at that snapshot; later failure
must still be included.

## Controlled runtime identity and gates

Source base: PR26 `0160644fa5a54f6fb6ab4d4b352190b38ca0d706` plus the hardware
adapter change. Actual ARM64 vehicle SHA256:
`ff5b4ff7e744b99ce4160ea697de66227e322d03bb1776e47e41af3e293882b6`.
Standalone signaling SHA256:
`c53e065fdcde5aa8a0a6d940f8cd824de5141f6f314cd676ffbc95804be6766c`.
Mac controller artifact source was `717a348`; outer artifact SHA256:
`9b37ff9547f726a38c3f747a345962aa0eccb5247a98c211443d5b1b89bcdc51`.
The same immutable Mac client, 11 UI assets, 1600x1000 headless Chrome viewport,
two cameras and configured 3D console were retained between A18/A19. A real
browser ran the actual keyboard handlers; no focus/visibility or input safety
checks were bypassed. Physical-display/render timing was not measured.

The harness cycles W/S/A/D, four diagonals and Space. It checks both video
freshness gates, authenticated actual DC command acceptance, mock D/R gear,
steering sign and brake feedback, zero throttle, zero speed/torque and a
five-degree session limit. Direct UDP selected pairs must match explicit
physical-interface endpoint allowlists and be connected/nominated/writable.
Both wall and monotonic spans must reach 3600 seconds in one session; failed
prefixes are not concatenated. Clock25ms, lease/gap200ms, watchdog800ms,
video500ms, consecutive-late3 and transient-hold-disabled remain unchanged.
VPN, Windows driver/settings and router settings were not changed.

## A18: original NX Wi-Fi power saving On

A18 failed during Space brake after 1065.122 wall /1065.1207 monotonic seconds,
after 43 complete nine-input cycles. Both video ages were 699.7ms; DC remained
selected, clock uncertainty was 9ms and sampled stop sequence was0. Raw logs
had two control-link braking-wait events, so the zero stop sample is not a pass.
Native publication cohort was DC20759/WSS1; accepted result RTT p50/p95/p99/max
was17/30/35/238ms. Publication supersession is not counted as packet loss.

The failure snapshot reported fresh encoder outputs, source RTP-output maximum
gaps of43ms, no encoded-queue backpressure and approximately29.9fps per lane.
Browser one-second receive samples showed approximately9.8%/12.9% packet loss,
then simultaneous displayed-frame expiry. These observations narrow the
incident beyond encoding, but do not prove physical radio transmission or
uniquely identify the Wi-Fi/AP/WebRTC queue component.

## A19: temporary NX interface power-saving Off

The designated NX Wi-Fi interface was originally On. A19 switched only this
interface to Off without restarting it; all other test settings and runtime
binaries are retained. An independent root systemd timer restores the original
On state at 11:51:10 UTC. Explicit restoration completed earlier at 10:49:57.473
UTC; readback confirmed On and the timer inactive. No persistent NetworkManager,
VPN, router or Windows changes were made.

A19 failed after 1384.043 wall /1384.0411 monotonic seconds, with 56 complete
nine-input cycles. During cycle57 Space brake the path changed from DC to WSS;
the final sample had no matching accepted DC intent. Video age was261.8ms,
clock uncertainty7ms and sampled stop sequence0. Raw logs contained control-link
and critical-video braking-wait events. Native publication counts were
DC26624/WSS5; accepted result RTT p50/p95/p99/max was17/35/39/220ms.
Disabling interface power saving did not meet the acceptance requirement and
does not establish the root cause. Failed prefixes are retained separately.

## A20: Windows headless Edge, original NX power-saving On

Headless Edge157 can execute the real UI and keyboard handlers through the
authorized Windows SSH account without an interactive desktop login. Its
1600x1000 viewport reports focused and visible; no product focus, input lease or
video freshness gate is bypassed. Browser creation and the bounded test run
share one persistent SSH session. A prior preparation attempt exited before
any scored input and is retained separately.

Windows retains original MediaTek driver3.0.1.1336. No manual NIC, power-plan,
VPN or driver change is made; the client's normal scoped Wi-Fi streaming API
still runs through its ordinary lifecycle. The immutable Windows controller
SHA256 is `90b3fcd8b9029ed9214effe6852bb7fee376bd49462a956f35a6f968f8a766b3`.
Both actual endpoint addresses are checked against physical-interface
allowlists. This is a control-path test, not visible GUI/render acceptance or
a controlled native-versus-web performance comparison.

A20 failed after462.647 wall /462.6527 monotonic seconds, after18 complete
nine-input cycles. Cycle19 failed during back-right: camera0 age537.1ms,
camera1 age386.9ms, driver state DEGRADED, clock uncertainty9ms, focused page
and sampled stop sequence0. All678 periodic samples still reported direct DC;
the actual selected pair was physical-endpoint IPv6/UDP host-to-host.
The complete raw scored window contained one control-link braking-wait event,
9048 DC /9 WSS native publications and one vehicle callback received through
the native signaling WebSocket. Zero sampled stop sequence is not a pass.

There were8974 unique recorded Worker DC sends, five without matched vehicle
callbacks and seven without correlated results; missing records are not
declared packet loss. Accepted-result RTT p50/p95/p99/max was21/34/60/112ms,
but vehicle callback command age reached535ms including rejected commands.
Sender wakeup lag max3ms, native send-call max1ms and vehicle mutex wait max1ms
do not establish the downstream queue/radio component responsible for the gap.

At the in-window braking-wait event, each lane had14000 encoded frames,
zero encoder backpressure/failure counts, last encoded ages8/14ms and maximum
RTP-output gap43ms. Each had discarded one stale AU and completed a requested
fresh-IDR recovery in32ms, with no outstanding IDR wait. This confirms the
adapter was working during real input traffic; RTP output is not physical NIC
transmission or receiver delivery evidence. It does not prove that hardware
encoding solves the control/network tail problem.

The harness sent real W/S/A/D combinations and Space; it did not merely observe
video. Its input-feedback audit confirmed18 complete nine-scenario groups and
the completed prefix of cycle19. Raw runtime safety intervention makes the
60-minute result FAILED. The owned client/browser and NX fixture stopped;
CAN remained down, both cameras were released, NX power saving remained its
original On, and Windows retained driver3.0.1.1336. No VPN or safety changes.

## Retained evidence and review

The adapter commit is `744518bbabbd2e0feb2343873b72a5d84ba57591` on PR26,
targeting PR25 (`rjr/cx/orin-isx031-cpu`). Its Build, Offline and Windows native
receiver CI workflows all passed. Reuse/quality/efficiency review retained the
existing backend and selection order, isolated vendor parameter differences
in the existing stage builder, and added no per-frame work or new queues.

Failed attempts are archived separately with member SHA256 verification and
credential/private-key scans. A18 archive SHA256:
`1e66df5e9a5da40ae3059c396fdad96784c05395551f993434cb185d5969cc9a`;
A19: `a7ae818e1a31e0cea33642ec3ee6a385fa355d4943e81dabea35c76b24cc306c`.
Original failed probe/setup receipts are retained. A20 Windows collection was
verified against its17-member hash manifest; full scored traces, rotated-log
support, original vehicle output and normalization receipt are retained.
A20 final archive SHA256:
`fe4bbf547309b9f306c776c0a16ffdd1efb0e2dc90f57d33ade0549ff7e6c5b1`.
All three archives were copied to project artifact storage and verified again.
Expired fixture credentials/private keys and credential-bearing CLI captures
were removed; user SSH keys were retained. Windows final readback confirmed its
original power plan, all14 saved NIC properties, driver1336 and no owned
remaining client process or power journal. Task-owned idle build containers
were stopped. No failure prefixes are concatenated to claim duration.
