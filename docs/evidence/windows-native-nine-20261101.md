# Windows native nine-input mock tests, 2026-11-01

> **Obsolete (2026-11-05):** This record compares runs that used the DataChannel/WSS control and telemetry copies, which have been removed from the code. Its comparison does not apply to the current implementation and is kept as history only.
> **已失效（2026-11-05）：** 本记录对比的 DC/WSS 控制与遥测副本已从代码中删除，对比结论不适用于当前实现，仅作历史参考。

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

A further controlled retry kept both cameras at 960x768 and 30 fps but reduced
the encoder target from 1,500 to 750 kbps per camera. It was stopped early
after safety waits made the 30-minute criterion unattainable. Of 21 completed
press windows, 4 had no matching accepted command; the vehicle logged 3 braking
waits. Around the first event, Windows-to-gateway probes had 2 timeouts and a
229.58 ms maximum, while Orin-to-STUN probes peaked at 33.49 ms. Lowering the
test bitrate therefore did not remove the observed failure in this short run;
the non-randomized run cannot establish that bitrate has no effect.

The experiment is opt-in for the scoped mock vehicle and is not enabled for
production control. A third independent access link, an improved WLAN, or a
separate field test is needed before drawing conclusions about a stable
30-minute deployment. Original logs and analysis outputs remain in the local
`mine-teleop-artifacts/pr26-dual-control-20261101` artifact directory; they
include timing data and must not be treated as physical execution evidence.

## Same-network retries and telemetry cadence

The user requested continued tests with the existing Windows Wi-Fi and separate
Orin 5G connection. Five subsequent baseline attempts were ended after a
braking wait made uninterrupted acceptance impossible. The five attempts
contained respectively 159, 176, 71, 158 and 101 input windows; 6, 3, 14, 3
and 2 windows had no matching accepted vehicle command. The vehicle reported
respectively 2, 2, 2, 2 and 1 braking-wait events during measurement. In each
case the first event coincided with Windows gateway timeouts or a major RTT
spike; contemporaneous Orin-to-STUN probes stayed responsive. These observations
identify a common Windows access path or host-scheduling problem, but do not
distinguish the Wi-Fi adapter, its driver, the operating system and the router.

The opt-in Windows diagnostic build then showed input authorization being
withdrawn while control remained otherwise ready because vehicle telemetry's
source timestamp exceeded the existing conservative freshness bound. The
vehicle normally emitted telemetry every 100 ms, leaving limited margin for
transport delay once the two clocks' uncertainty was included. PR26 now emits
a new telemetry snapshot at each approximately 50 ms watchdog tick **only**
for the explicitly enabled zero-speed dual-path mock. It does not change the
freshness threshold, input release requirement, command deadline, or production
telemetry interval.

In the first run with that change, all 162 press windows matched at least 50
accepted commands, including 18 windows for each of the nine inputs. The run
still ended after about 16 minutes with one control-link braking wait: during
the surrounding four-second interval, Windows gateway probes had three
timeouts while Orin STUN probes had none. Thus the input result is encouraging,
but this run did not pass the 30-minute zero-stop requirement. The baseline and
changed runs are consecutive rather than randomized; they cannot by themselves
establish causation or production stability. Effective traction and steering
remained zero throughout the analyzed mock inputs.

In the next run, all 60 input windows also matched at least 50 commands, but
two braking-wait events occurred after about six minutes. Near the first event
Windows gateway probes timed out twice while Orin STUN probes had no timeouts.
This second run likewise did not pass uninterrupted-control acceptance.

The next attempt ended after about 20 seconds with one braking wait; both
completed input windows matched accepted commands. The following attempt
completed 1,800.216 seconds with no vehicle braking-wait events. All 300 press
windows matched at least 72 accepted commands. Forward, reverse and left were
each exercised 34 times; right, the four diagonal combinations and brake were
each exercised 33 times. The vehicle accepted 21,919 matching commands via
DataChannel and 868 via the authenticated WSS duplicate. The vehicle rejected
duplicate copies rather than applying them twice. All effective traction and
steering values in the matching commands remained zero.

All 1,789 one-second client observations reported the expected direct UDP SCTP
pair, the logical DataChannel control path and zero safety-stop episodes. The
client's accepted-command result RTT was p50/p95/p99/max 39/52/58/90 ms over
35,088 samples; this is round-trip result latency, not one-way command latency
or physical actuation. Three observations reported the remote media status as
expired even though no control stop occurred; uninterrupted remote-media
freshness is therefore not established by this run. Windows gateway probes
had seven isolated timeouts during the successful window, showing that an ICMP
timeout alone cannot prove a control-path outage or identify a faulty device.
The previous failed windows remain part of the evidence: a single passing run
does not establish durable reliability under this Wi-Fi/5G combination.

This completed run used Windows viewer commit `83c2528` and Orin runtime commit
`26c00d0`; subsequent commit `e7a1c72` changes only a test fixture. The Orin
runner verified the cellular route, mock zero-speed configuration and CAN-down
state, and reported no guard failure. Test-scoped cloud keys and credentials
were removed afterwards; CAN remained down and both video devices were free.
