# Orin NX hardware encoder and mock control, 2026-11-17

Status: Jetson hardware encoding is operational. A18 failed; A19 is still in
progress and has no acceptance result yet. Windows GUI testing is unavailable
because the user cannot log into its desktop. These are Mac headless Chrome
and NX same-LAN tests, not Windows, physical display, WAN/5G or vehicle acceptance.

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

The designated NX Wi-Fi interface was originally On. A19 switches only this
interface to Off without restarting it; all other test settings and runtime
binaries are retained. An independent root systemd timer restores the original
On state at 11:51:10 UTC, and explicit restoration/readback is required after
the test. No persistent NetworkManager, VPN, router or Windows changes were made.

A19 result and final restoration are pending; no 60-minute pass is claimed.
An eventual pass under this condition would not, by itself, prove the cause of
A18 or establish WAN/5G or Windows reliability. Full raw transport, safety and
input-feedback logs must be audited beyond the periodic UI samples.
