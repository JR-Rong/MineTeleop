# Mine Teleop Protocol v1

Every session-scoped message uses this required metadata envelope:

```json
{
  "protocol_version": 1,
  "vehicle_id": "vehicle-001",
  "driver_id": "driver-001",
  "session_id": "session-001",
  "seq": 1024,
  "sent_at_utc_ms": 1780000000000
}
```

Rules:

- `protocol_version` must equal `1`; incompatible versions are rejected.
- `vehicle_id`, `driver_id`, and `session_id` are non-empty strings.
- `seq` is a positive, monotonically increasing integer within its message
  stream.
- `sent_at_utc_ms` is a non-negative UTC Unix timestamp in milliseconds.
- Unknown fields are ignored so additive changes remain compatible.
- Missing required fields or fields with the wrong JSON type are rejected.
- A `control_command` additionally requires a non-empty `control_token`. The
  receiver validates vehicle, driver, session, token, sequence, and freshness
  before actuation.

`control-command.valid.json` is the shared golden vector for vehicle, control,
and server builds. `control-command.invalid-missing-driver-id.json` is a shared
negative vector.

The shared session-state vocabulary is:

```text
offline
online
reserved
connecting
active
degraded
stopping
closed
```

`offline` and `online` describe vehicle presence. A control session progresses
through `reserved`, `connecting`, `active`, optional `degraded`, `stopping`,
and `closed`. Closing a session clears its control token before any later
session can be created.

## Media protocol 2 safety extension

The base command version remains 1. Every driving command and native intent in a media-protocol-2 session carries the vehicle-issued `control_epoch`; profile/VCU handshake requests and their confirmations carry both `control_epoch` and `media_attempt_id`. The vehicle rejects missing/old epochs before actuation. Changing epoch clears retained input and requires a fresh neutral intent followed by new operator input. `media_quiesce`/ack use a unique `request_id` over authenticated WSS; no page state may attest to parking. Critical camera faults remain latched for the same cloud session. See [surround protocol and frame provenance](../../docs/surround-and-relay.md).
