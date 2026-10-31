# Windows / Orin 5G mock control: 2026-10-31

**Result: the requested uninterrupted direct-control/no-stop acceptance failed.**
These are zero-speed mock tests with CAN down, two critical cameras, unchanged
25 ms clock uncertainty and 200 ms input lease, Windows WLAN and Orin 5G.
Signaling was isolated behind endpoint-bound, restricted SSH tunnels. The
selected IPv6/UDP WebRTC pair was checked against both public endpoints; the
management cable and tunnels are not the measured direct media path. This is
not physical actuation, a real vehicle, optical display latency, or a matched
browser comparison.

## Results

| Run | Result |
| --- | --- |
| Baseline short | 60.98 s measured; no braking wait during that window, one during startup. Old Windows viewer, used only to check the setup. |
| First long | Ended after approximately 1,275 s of measurement following a control-link braking wait. Affected DataChannel callbacks had estimated command age up to 913 ms. The mock client stopped when the vehicle profile became inactive. |
| Diagnostic retry | Did not reach active control in the 120 s startup window: three DC trials failed, WSS remained selected, and the vehicle never logged a received profile request. No 30-minute window began. |
| Final long | 1,800.418 s measured. Three vehicle control-link braking waits, all followed by recovery; one started before the measurement window. The requested no-stop criterion failed. |

During the final window, 1,786 of 1,791 one-second observations reported DC
control and five reported WSS; all 1,791 reported a direct UDP SCTP media pair.
These are sampled counts, not continuous path proof. Three braking waits
started at approximately 932.823, 934.459 and 938.123 s into the window and
were followed by recovery approximately 1.350, 0.664 and 0.400 s later. The
client observed only two episodes; vehicle events are the authoritative count.

For the final window, DC vehicle-result RTT samples (n=34,842) had
p50/p95/p99/max 40/56/67/321 ms; WSS samples (n=58) had 65/294/325/325 ms.
These are received-result RTTs, so missing results are not represented.
The estimated driver timestamp to vehicle callback for DC (n=35,139) had
p50/p95/p99/max 16/22/23/286 ms and depends on synchronized-clock uncertainty
(maximum observed client uncertainty 16 ms). These figures are not physical
end-to-end or display latency. Input keys were exercised before the measured
window; the window itself was predominantly 20 Hz neutral mock control.

## Delay evidence and limits

For the first long-run failure, Windows reported enqueue completion in the
local DataChannel sender, while consecutive vehicle callbacks arrived with
estimated ages of 913, 864, 836, 786, 736 and 687 ms. The vehicle rejected
expired commands and then entered braking wait. An Orin AF_PACKET capture
showed a roughly 513 ms gap in inbound DTLS records, later a roughly 530 ms
gap, and a burst of larger DTLS records at essentially the same local time as
the delayed callbacks. The capture cannot associate encrypted DTLS records
with individual SCTP messages or distinguish Windows transmission, cellular
network, modem, USB driver and other pre-callback stages. A separate Orin-to-
cloud IPv4 ICMP sequence also had a missing response around that event; this
does not prove a common cause. During the final run's braking events the
one-second cloud ICMP samples did not show an equivalent spike.

The bounded Orin capture ran for 3,600 s and reported 92,300 matching DTLS
records, no missing kernel timestamps and no packet-socket drops. It stopped
automatically during the final run, so later final-run events have no Orin
kernel packet timestamps. Windows pktmon used a two-endpoint IPv6/UDP filter,
40-byte packet prefix and a 128 MiB circular log; it reported no lost events.
Packet metadata is diagnostic and the additional capture load prevents a
strict performance comparison with prior uninstrumented runs. No application
payload was deliberately recorded. Raw logs and exact artifacts are retained
in the private local test artifact directory.

The mock-only changes bound profile-delivery retries to the identical request
and sequence, and permit at most one additional idle DC retry after the normal
budget, with fresh neutral ownership, healthy WSS/video and clock gates. The
final startup succeeded on the first profile attempt, and the extra idle retry
was not exercised in this WAN run. Neither result proves that the intermittent
delay or production WSS fallback is fixed.

Windows CI and the offline Linux suites passed; local mock-gate tests and 87
vehicle/core cases passed. The macOS CI job failed: two runs reported a direct
withdrawal deadline failure, while a later run passed that test but failed an
HTTP lease test after an observed 218 ms input gap against the unchanged 200 ms
lease. The same direct integration test passed three times on the local Mac;
the CI failures remain unresolved. Earlier failure logs were retained.

The Windows test viewer came from commit `024dbaf811411cba9b0d811c7a309719eaddd0b4`
(SHA-256 `863be8cc22e9e15d619ba004e7d834628acbe8c0e11ecf83c540d6dc1d4ef65c`).
The Orin ARM64 test binary was built from `f001af8c92e9118c5c7f7b9fb66425d87a013324`
(SHA-256 `a2a40ba91a902fc7b2267de7a7273396dc50723a85f4b93e31ab9bc65bef6dd2`);
the later commit changed only Windows mock code and its tests, not the Orin
production sources. A subsequent commit changed test diagnostics only.
All temporary test processes, restricted cloud public keys, credentials,
Windows scheduled tasks and packet-monitor filters were removed; CAN remained
down and both cameras were released.
