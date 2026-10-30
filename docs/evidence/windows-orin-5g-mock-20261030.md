# Windows–Orin 5G mock-control observations, 2026-10-30

This is a bench observation, not vehicle or CAN acceptance. The vehicle used the
mock adapter, two physical ISX031 cameras and a zero-speed profile; `can0` stayed
down. Orin's cellular interface and Windows WLAN routes were checked separately.
The selected browser media pair used the specified global IPv6 endpoints and UDP;
there was no TURN server in this isolated run. Signaling used restricted SSH
forwarding. Both sides sampled time on the same cloud host as the isolated
signaling process; Windows used a loopback-only, TLS-verifying HTTPS proxy bound
to WLAN. In the first browser window, vehicle time requests used the signaling
tunnel; the subsequent native and browser windows used a separate TLS-verifying
HTTPS time route bound to Orin's cellular interface. The native and vehicle
safety thresholds were not relaxed.
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

The subsequent instrumented native short run isolated one avoidable source of
sender starvation: identical neutral refreshes changed `intent_seq` on every
heartbeat. Because the sender correctly revalidates prepared input before IO,
refreshes coinciding with preparation repeatedly retired otherwise unchanged
snapshots. The observed run recorded 17 `prepared_input_retired_no_io` skips,
including a consecutive sequence before a 945 ms successful-send gap. The
vehicle then reported the session profile inactive, and the native client
stopped. The fix keeps the same sequence for identical mock heartbeats while
advancing it whenever scope, safety evidence or input content changes. It does
not alter the dispatch recheck, command expiry or the input lease.

## Native 30-minute window

The Windows executable was built from `8c05b4ab0981f9276832fedfa2b9d749fb46733a`;
the Orin and isolated signaling binaries used the same vehicle/signaling code
as `5e2ecb5dbd04910d991df2312a31f0dd0268107e`. A single foreground observation
lasted 1800.37 seconds, with ongoing cellular route checks and `can0` down.
The native client provides a zero-input mock control path, not a production
driving interface. The selected media pair was the specified IPv6/UDP direct
pair. The control path used DataChannel except for startup and a roughly
five-second WSS fallback, after which it returned to DataChannel.

There were two distinct braking/degraded episodes; the stop sequence remained
zero. Around the first episode the vehicle received commands approximately
340 ms apart and rejected several expired commands. All 28,606 attempted local
DataChannel sends were queued; that does not establish timely network delivery.
The traces do not isolate the cause among scheduling, SCTP and the network.
Remote media readiness also expired briefly twice later, without another counted
episode. Consequently the observation does not meet an uninterrupted
no-intervention acceptance criterion.

Resampling the latest native software-result round trip at the one-second
observation cadence yielded 1,784 DataChannel samples: p50 32 ms, p95 41 ms,
p99 47 ms and maximum 79 ms. Five WSS samples had p50 94 ms and maximum 258 ms;
the sample count is too small to characterize tails. Result return paths may
differ from command paths. This sampling can miss short spikes and is not
camera-to-display or one-way latency. GDI submission interval p99 was 51.0 ms
and 50.7 ms for the two streams, with maxima 647.5 ms and 661.7 ms; this is not
physical scan-out or directly comparable to browser decoder freeze statistics.

## Bench time-proxy failures and correction

An additional browser attempt lost its original peer after approximately 149
seconds. Before the re-negotiation, the vehicle reported an HTTP response parse
failure, and the test-only Orin time proxy logged an assertion: a persistent
connection initially used for `/time` had subsequently carried a different
request. This is an invalid 30-minute observation, not evidence that the media
route alone failed. A first correction closed both sides of every time request;
the ensuing TLS handshakes led to a measured 44 ms time uncertainty and the
vehicle correctly rejected startup at the unchanged 25 ms limit. A further
test-only correction isolated downstream requests while reusing the upstream
TLS connection, still bound to the cellular interface. Consecutive preflight
requests then measured about 20–23 ms after initial connection setup, and a
subsequent ordinary HTTP request succeeded. This proxy is bench infrastructure,
not a production time synchronization design.

The first browser attempt with the corrected proxy lost its session at about
1519.7 seconds, so it was not a continuous 30-minute run. Before the disconnect,
the browser reported `ERR_NO_BUFFER_SPACE` for requests to its local control
service, including the signaling poll; the page then withdrew control. The
vehicle reported that the session was no longer active. These observations do
not establish which Windows resource was exhausted or prove the media route
caused the session loss. Before that, there were two braking-wait transitions
and three degraded transitions in the telemetry poll, plus a roughly ten-second
control fallback from DataChannel to WSS; the media pair remained direct while
the session was active. The stop sequence remained zero. This interrupted
attempt is retained separately and is not combined with another window.

A further attempt ended after approximately 1599 seconds with the same
`ERR_NO_BUFFER_SPACE` on the local signaling poll. Its telemetry poll recorded
five braking-wait transitions and six degraded transitions before the session
ended; stop sequence remained zero. Media was direct while the session was
active, while control fell back to WSS near 24 minutes. The Windows host TCP
sampler observed a maximum of about 7,200 `TIME_WAIT` connections, and a
separate sample found about 7,100 associated with the local control service
port. The local HTTP server explicitly closes each response. This connection
churn is a concrete investigation lead, not proof of the resource that caused
the browser errors; at the sampled time nearest the disconnect, about 5,700
connections were in `TIME_WAIT`. This attempt also does not count as a full
30-minute window.

## Browser 30-minute window with the cellular time route

After the interrupted attempts above, a new single browser window ran for
1800.040 seconds, 12:59:29–13:29:29 UTC. The two cameras captured at
1920x1536 and were encoded at 960x768, 30 fps. All 1,793 one-second samples
showed the same connected peer, the designated global IPv6/UDP host pair, a
foreground page, both critical videos ready and an acknowledged mock profile.
The pair stats reported `in-progress` in 17 samples. Orin's guard reported no
route or CAN failure and `can0` remained down.

Control used DataChannel for 1,359 samples; at 1365.2 seconds it fell back to
WSS for the remaining 434 samples. The 50 ms telemetry poll, started 4.54
seconds into the window, saw one braking-wait transition and one degraded
transition. Vehicle logs around this event record two entries into braking wait,
with 24 ms between the first recovery and the second entry; polling can merge
closely spaced events. The vehicle observed command arrival gaps of 324–336 ms
and rejected expired commands on both DataChannel and WSS. `stop_sequence`
remained zero. Thus the window completed, but uninterrupted direct control and
no safety intervention were not achieved.

One-second software-result round-trip samples grouped by command path were:

| Command path | Samples | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| DataChannel | 1,360 | 40 ms | 55 ms | 61 ms | 67 ms |
| WSS | 433 | 54 ms | 65 ms | 75 ms | 277 ms |

These include the return path, may miss short spikes, and are not one-way or
camera-to-screen latency. The two browser decoder tracks reported 53,987 and
53,984 frames, with six freezes totaling 1.848 and 1.831 seconds respectively.
The native GDI submission intervals are a different measurement and must not
be treated as directly comparable display latency. The 30-minute browser and
native tests were sequential, had different interface implementations and used
different versions of the test-only time proxy; they do not establish a causal
latency benefit for either client.

There were 11 browser `ERR_NO_BUFFER_SPACE` errors to local endpoints during
the measured window, although none ended this session. The Windows TCP sampler
observed 5,743–7,224 `TIME_WAIT` connections. This does not identify the exact
resource behind the failures, and the preceding interrupted attempts remain
material reliability evidence. The browser used an explicit mDNS-disabling
test flag and is not proof of default-browser direct connectivity.
