# Windows native nine-input mock tests, 2026-11-01

These results are for an isolated `test_bench_` session with the mock adapter,
CAN down, two cameras, zero speed, zero torque and zero steering limit. The
Windows client used Wi-Fi; the Orin media route used its separate 5G IPv6
interface. A signaling tunnel was used for the isolated cloud service. The
selected SCTP pair was checked against the expected direct UDP endpoints.
These results do not establish real-vehicle, physical motion, display scan-out,
or production safety acceptance.

The harness repeatedly pressed W, S, A, D, W+A, W+D, S+A, S+D, and space for
four seconds each, with releases between cases. Analysis requires a matching
accepted vehicle command in the corresponding press interval. The hard speed,
clock, lease, video freshness and control watchdog gates were not relaxed.
Both runs used the same Windows application built from `3843e61`; the only
subsequent product runtime difference was the vehicle telemetry mirror. The
vehicle binaries were built from `53b0c43` and `dbafa2a` respectively. Commit
`4631142` changes only a test failure message compared with `dbafa2a`.

| Measurement | Control mirror only | Control and telemetry mirrors |
| --- | ---: | ---: |
| Duration | 1,800.6 s | 1,182.7 s; ended on safety failure |
| Input windows | 300 | 197 |
| Windows with no matching accepted command | 11 | 0 |
| Windows with fewer than 50 matching accepted commands | 75 | 9 |
| Vehicle control braking-wait events | 4 | 1 |
| Result RTT p50 / p95 / p99 / max, ms | 39 / 59 / 63 / 511 | 39 / 51 / 60 / 533 |
| Matching commands with nonzero effective traction or steering | 0 | 0 |

The 50-command count is a descriptive indication of reduced coverage during a
four-second press, not an agreed safety acceptance threshold. Cross-host event
window matching is subject to the time-sync uncertainty; the closest boundary
samples may be classified differently. The second test ended with inactive
control parameters after the vehicle's command-age safety gate intervened; the
client closed the session. Neither run passed the 30-minute zero-stop criterion.
These are consecutive runs, not a randomized experiment: different Wi-Fi/5G
conditions preclude attributing the difference to the telemetry change alone.

During the second run's safety event, Windows-to-gateway probes had two
500 ms timeouts and Windows-to-public-STUN probes had timeouts and RTTs up to
330 ms, while the Orin-to-STUN samples during the same period were approximately
17–29 ms. In the first run a gateway spike and simultaneous late direct/WSS
results were also observed. This is strong evidence of a shared Windows access
path or host scheduling problem, not proof identifying the AP, adapter, driver,
or scheduler. DC and WSS share that path and cannot guarantee resilience against
its complete outage. In both experiments WSS is the existing authenticated
signaling path; it is not a separately allocated TURN media relay.

The experiment is opt-in for the scoped mock vehicle and is not enabled for
production control. A third independent access link, an improved WLAN, or a
separate field test is needed before drawing conclusions about a stable
30-minute deployment. Original logs and analysis outputs remain in the local
`mine-teleop-artifacts/pr26-dual-control-20261101` artifact directory; they
include timing data and must not be treated as physical execution evidence.
