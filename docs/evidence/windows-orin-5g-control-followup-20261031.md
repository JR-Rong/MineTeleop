# Windows / Orin 5G control follow-up, 2026-10-31

This is mock-only evidence. The Orin used two physical cameras and the mock
adapter at zero configured speed. CAN remained down; its media route to the
Windows WLAN endpoint used the cellular IPv6 interface. VPN settings and the
existing safety thresholds were not changed. The isolated cloud signaling
service and time path were separate from production services. Packet tracing
adds overhead, and these results are not a real-vehicle, physical display
latency, or production availability acceptance.

## Existing JSON control, 1,800 s

The Windows viewer was built at `b8f73760d6753c2b0c5b564c466b38db9817e3bb`
(SHA-256 `508117cdcab65e20c5386f7d3bdc967e3241936afff2da4edd34d552fcabb5e9`).
The Orin binary used matching production sources (SHA-256
`50049b7082b2690fa28595982deb25cb5f4bd38786efc5944d0278b51217da52`).
PR commit `a53a46a` changed only an integration-test fixture relative to those
production sources. The measured window lasted 1,800.151 s, with fresh native
mock input before the window and five brief mock inputs during it.

**Acceptance failed:** the vehicle recorded three control braking-wait entries
at approximately 314.9, 315.5 and 320.4 s into the window. The native sender
published 35,142 commands on DataChannel and 97 over WSS. The vehicle logged
35,137 direct callbacks, 97 WSS callbacks, five expired commands and ten
replayed commands rejected. Every one-second media-path sample reported direct
UDP, but five control-path samples reported WSS: sampled ICE path does not prove
continuous direct control. The synchronized client reported at most 15 ms time
uncertainty. DataChannel vehicle-result RTT p50/p95/p99/max was 40/55/67/732 ms.
The timestamp-based sender-to-vehicle callback estimate was 17/23/26/720 ms;
it depends on clock synchronization and is not physical one-way latency.

The Windows outgoing DTLS probe recorded 90,755 records in the window; 90,753
were matched by epoch, record sequence, type and approximate session time to
Orin kernel receive records. The two unmatched records remain unaccounted for.
The full capture reported zero packet-socket drops and no missing kernel
receive timestamps. It records DTLS headers, not SCTP payload or command
identity. Around the fault, command enqueue continued every approximately
50 ms while outgoing DTLS records had gaps of about 363 ms and 576 ms; matched
records did not show comparable additional delay between the instrumented
Windows WebRTC send point and Orin kernel arrival. This localizes this event to
before that send point but does not isolate a particular Windows thread,
GStreamer component or SCTP congestion mechanism.

## Isolated SCTP experiment

An in-process GStreamer 1.20 loopback withheld the receiver's outgoing DTLS
records for approximately 900 ms while the sender produced 20 messages/s.
With about 950 bytes of message padding, 14–17 messages arrived more than
200 ms old in repeated trials despite the channel reporting buffered amount
zero. An experimental 120 ms SCTP lifetime, including a local byte-order
compensation probe, also left 15 stale messages in this model. No such probe or
lifetime change was integrated into the product. With approximately 150 bytes
of padding, the same single trial produced no messages older than 200 ms;
approximately 350 bytes produced nine. This supports testing smaller messages,
not a guarantee about real networks or loss patterns.

The subsequent compact CBOR control experiment is enabled only by an explicit
vehicle process environment flag, a `test_bench_` identity, mock adapter and
zero maximum speed, and is negotiated only with the native mock viewer. It
reconstructs the complete original JSON before the existing MAC, session,
sequence, freshness and control gates. Unknown signed fields fall back to JSON.
Local tests cover exact signed serialization, byte-wise tampering, cross-session
replay, truncated input, scope restrictions and GStreamer binary delivery.
Real 5G 30-minute acceptance for the compact experiment is **pending**.

Raw logs, capture summaries, binary hashes and local probe scripts are retained
in the private `pr26-iter2-20261031` artifact directory. This report does not
include credentials, raw packet payloads or peer addresses.
