# Stop-only runtime checkpoint

This commit builds on `67d90b6`. It implements the stop-only exception described
in the direct-path review, behind the existing disabled experimental direct
authority flags. There is no deployment, command-line/config-file enablement,
new ordinary-command producer, or change to a safety timeout. The 600 ms lease
in the localhost fixture remains an unapproved test value.

## Runtime behavior

The native producer creates one normalized, authenticated emergency event:
`estop=true`, zero throttle/steering, full brake and inactive operator. Its
existing global sequence and intent sequence, authority issuer/epoch/lease
reference, media attempt and emergency reset epoch are signed together. The
same serialized command is published to an independent, bounded local bridge
before this event's WSS connect/drain/write. The browser can copy those exact
bytes to an open DC while native attempts WSS. Ordinary commands retain the
existing single selected path.

The dedicated bridge neither takes the ordinary transport update lock nor
performs network I/O. It validates current session, native generation, page
owner/epoch and media attempt. One browser request and one pending result are
allowed. The existing 100 ms local publication allowance and existing DC
buffer threshold bound forwarding; they do not extend vehicle command age or
define cloud failure detection. No retry re-signs or re-times a command.

The vehicle's `EmergencyStopReceiver` runs under the common control-service
mutex. It checks the full session/token identity, native MAC, normalized
stop-only fields, authority and reset epochs, original command age, and DC
media attempt. A current owner's stop may use an already expired short grant
reference; it cannot renew that grant or authorize motion. This is the only
new DC exception to ordinary transport-epoch selection.

The fixed 64-entry pending/result cache applies an event once across DC/WSS.
Duplicates return the same signed result without calling the control service
or refreshing its watchdog/lease. Sequence conflicts and evicted old events
are rejected. A real Estop-to-reset transition advances the emergency reset
epoch before cache lookup, so an old cached event cannot relatch after reset.
An old session/token or retired authority also cannot be reused.

Software results include the actual vehicle receive carrier. Native measures
from the event's first local publication to its first authenticated exact
vehicle result. A cached DC result returned over WSS remains a DC-received
command; it cannot manufacture a faster WSS command RTT. These bounded dual
stop events are excluded from ordinary path-comparison RTT samples.

The UI only displays fresh authenticated **software Estop latch** confirmation
from native. It explicitly reports `physical_execution_confirmed=false` and
`covers_all_commands=false`. Fresh CAN/actuator evidence is still needed for
physical confirmation. Ordinary cloud ingress ACKs remain insufficient.

## Explicit limits

The native periodic producer must first reach and sample the emergency intent.
This bridge avoids this event's subsequent WSS block; it cannot preempt an
earlier already-blocked ordinary WSS operation. It is not a proof of minimum
physical stopping latency or complete emergency preemption. The independent
vehicle watchdog and short-authority expiry remain in force.

The existing shared WebRTC association can itself be impaired. An open DC is
only an available bounded stop attempt, not evidence of healthy command
delivery. Independent ordinary feedback selection remains outside this commit.

The vehicle renewal RPC budget now follows the negotiated server duration,
rather than a potentially larger local maximum. No lease default or approved
duration is introduced.

## Local validation on 2026-10-15

- Native/cloud runtime and integration target compiled on macOS.
- CTest: emergency receiver, direct authority, ordinary transport, JS courier
  and ICE page targets: **5/5 passed**. Receiver tests use the real MAC and
  common control service, including dual delivery, reset/replay and a 512-event
  fixed-cache exercise. Courier suite contains 10 cases.
- Existing page control logic: **98 passed**.
- Actual vehicle ICE signaling methods extracted and compiled against fake Gst
  primitives: compile/run passed; this is not full GStreamer ABI validation.
- Actual localhost native/cloud integration: **1/1 passed in 4.20 s**,
  08:45:19–08:45:23 UTC. It includes cloud outage/independent grant expiry and a
  700 ms stalled local WSS upgrade: the already-published emergency is delivered
  via the production bridge and receiver, and an authenticated software result
  is returned before the WSS stall finishes. Tampered feedback is rejected.
- `git diff --check` passed.

Evidence logs are in the delegating task directory as `emergency-*-final.log`.
No bench, production cloud, Mac UI, network settings or credentials were used.
Full Linux/GStreamer compilation and coordinated field checks remain required;
the local machine lacks `gstreamer-webrtc-1.0`. The blocked Docker environment
has not been reset or modified.
