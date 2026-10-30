# Windows–Orin 5G mock-control observations, 2026-10-30

This is a bench observation, not vehicle or CAN acceptance. The vehicle used the
mock adapter, two physical ISX031 cameras and a zero-speed profile; `can0` stayed
down. Orin's cellular interface and Windows WLAN routes were checked separately.
The selected browser media pair used the specified global IPv6 endpoints and UDP;
there was no TURN server in this isolated run. Signaling used restricted SSH
forwarding. Both sides sampled time from HTTPS `/time` on the same cloud host as
the isolated signaling process; Windows used a loopback-only, TLS-verifying
proxy bound to WLAN. The native and vehicle safety thresholds were not relaxed.
The dedicated test browser disabled host-candidate mDNS obfuscation to make its
IPv6 host candidate usable across the WAN; this is not a default-browser result.

## Browser 30-minute window

The signaling service, Windows control runtime and Orin vehicle code were built
from `5e2ecb5dbd04910d991df2312a31f0dd0268107e`. The 1800.32-second observation
contained 1,794 one-second samples. All samples reported a connected peer,
foreground page, ready critical video, and the designated selected UDP host pair.
The selected pair's stats reported `in-progress` on 15 samples, so those samples
are not proof that every connectivity check remained succeeded.

Control used the DataChannel for 607 samples and fell back to signaling WSS at
about 609.5 seconds; it stayed on WSS for the remaining 1,187 samples. Two sampled
braking-wait states were observed, one associated with control-link recovery and
one with time synchronization. The cumulative vehicle stop sequence remained
zero. Additional telemetry-event and 50 ms polling monitors began after 492 and
818 seconds respectively, and observed one braking transition each; the latter
also observed one degraded transition. Their coverage does not extend to the
beginning of the run.

During the run, one-second samples of the native dispatch-to-vehicle-result
round trip, grouped by the actual command path, were:

| Command path | Samples | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| DataChannel | 608 | 40 ms | 56 ms | 63 ms | 420 ms |
| WSS | 1,186 | 58 ms | 71 ms | 84 ms | 303 ms |

These are sampled software-result round trips and may use a different feedback
path. They are not one-way network or camera-to-display latency. Around the
control fallback, vehicle trace records showed approximately 300–350 ms gaps and
expired commands were rejected. The precise source of the delay was not proven.
The browser reported 53,989 and 54,001 decoded frames during the window; each
track reported seven freezes totaling approximately 2.20 and 2.24 seconds. These
are browser decoder/playback statistics, not physical display scan-out evidence.

Twelve one-second samples reported a vehicle time-sync candidate uncertainty
above 25 ms, with a maximum of 43 ms. Vehicle diagnostics around the later
intervention reported three consecutive anomalous cohorts and a time-sync
braking wait. Candidate uncertainty, safety readiness and actual brake/stop
transitions are separate observations; the 25 ms limit remained unchanged.

## Native short tests and remaining acceptance

The native window, two actual decoded video streams, scoped input lease, signed
control courier and SCTP DataChannel were connected to the same mock bench over
IPv6/UDP. Short tests reached `CONTROL_ACTIVE`, but observed repeated braking or
degraded states and eventually a profile-status validation failure. One recorded
sender gap reached 376 ms. The runner now requires ongoing control observations
and fails when the internal session exits; a still-open window is not accepted
as an active session. These short runs do not add up to a continuous 30-minute
native result and do not establish safe recovery or absence of intervention.

A follow-up bench-only clock route binds its TLS time connection to Orin's 5G
interface and reuses the same cloud host's certificate-verified `/time` endpoint.
Ten idle preflight cohorts measured 17–21 ms uncertainty; this is not a sustained
or loaded 30-minute acceptance. Native failure classification and opt-in sanitized
control tracing are being used to locate the sender gaps without extending any
safety lease or allowing nonzero mock input.
