# Windows / Orin 5G native compact-control and full-telemetry test

This is one isolated mock-only window, not real-vehicle acceptance. Windows
used Wi-Fi, Orin used its independently routed cellular IPv6 interface, and the
native viewer checked the exact non-relay UDP endpoints. Two physical cameras
were configured for 960×768 at 30 fps and 1,500 kb/s per stream. The mock
adapter had a zero-speed configuration; CAN remained DOWN. The 25 ms clock
uncertainty limit and existing video, input, command freshness and safety
gates were unchanged. VPN settings were not changed. Separate IPv4 STUN and
Windows gateway ICMP diagnostics began after the window started and added load;
these probes did not carry media or control.

The Windows native viewer was built from `859a33f` (SHA-256
`0bb411b2e243ee2cc63933d289fdfe44361dca43f8366fb22cecb172bb7b1df2`),
and the Orin production sources were `360da70`. The already existing
explicitly enabled compact-control experiment reconstructed the signed
command before the unchanged MAC and admission gates. This experiment is
restricted to the test-bench identity, mock adapter and zero speed. Full
telemetry remained enabled: the separate compact-telemetry option was off.
This test did not introduce or validate a production control encoding.

## Result

The formal window completed 1,800.239 s. **Acceptance failed:** the vehicle
logged three control-link braking-wait entries at 306.717, 308.366 and
311.967 s, each followed by recovery. A startup braking-wait and recovery
before the formal window are retained separately. No route guard failed, and
all 1,791 sampled media observations reported the selected direct UDP pair.
However, the native control path had 1,786 DataChannel and five WSS samples;
sampled transitions were DC → WSS at 307.448 s and WSS → DC at 312.479 s.
The sender logged 35,150 DC and 97 WSS sends. The vehicle logged 35,149 DC
and 91 WSS callbacks and rejected 12 expired commands; differences in counts
alone are not proof of packet loss.

The viewer recorded 34,947 DC result RTTs, with p50/p95/p99/max of
39/54/64/327 ms. These are software result round trips, not physical
camera-to-display or actuator latency. The vehicle's synchronized estimate
for DC sender timestamp to callback was p50/p95/p99/max 16/22/23/334 ms;
cross-host clock uncertainty applies. Of the 35,149 recorded compact DC
publications, the wire size had p50/p95/p99/max 79/79/79/83 bytes. The
earlier comparable JSON run used approximately 926 bytes per command.
Shrinking commands alone did not avoid this window's late-control failure.

Five scheduled Space exercises yielded 23, 23, 25, 22 and 24 accepted
positive requested-brake commands respectively, with requested brake 0.3.
During these samples the existing recovery safety hold kept effective brake
at 1.0. Thus request acceptance does not demonstrate unheld actuation.

Around the first braking cluster, vehicle callback records showed multiple
compact DC commands sent about 50 ms apart arriving within milliseconds;
three were rejected as expired with estimated sender-to-callback delays of
334, 285 and 238 ms and no reported callback mutex wait. After fallback,
several WSS commands were also late and rejected. In the surrounding 11 s,
the independent Windows-to-cloud STUN probe peaked at 268 ms, Windows gateway
ICMP peaked at 284 ms with two failures, and Orin-to-cloud STUN peaked at
36 ms with no timeout. Those probes use different destinations and IPv4;
ICMP may be deprioritized. Their correlation suggests investigating the
Windows Wi-Fi or shared path, but does not establish the cause or identify
the segment responsible for the IPv6 command delay. No packet-level capture
was performed in this run.

The bounded native fallback recovered to DC in this instance; it does not
prove recovery after repeated failures or a network change. A prior
compact-control **and compact-telemetry** window passed, but used a different
workload and does not establish that this full-telemetry run would pass.
The temporary test credentials and restricted SSH keys were removed after
the run. Other raw logs and test scripts remain outside the repository.
