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

Ordinary native control commands additionally require Boolean
`control_time_sync_ready`, `operator_input_ready`, and
`recovery_sample_healthy`, plus non-negative integer
`recovery_bad_sample_revision` and `recovery_brake_revision`. The Boolean reports current raw sample health;
it must not be replaced with an anomaly-tolerance gate. A legal authenticated
ESTOP remains independent of these newer fields. Metadata, identity, token,
and numeric actuation validation still apply to ESTOP.

The revisions preserve events across latest-value coalescing: a producer's new
bad sample advances bad history; a third consecutive anomaly or independent
absence advances brake history. A newer normal sample clears only its category's
current counter, never that history. A changed bad revision resets an existing
shared 300ms health window; a changed brake revision starts braking wait even
when the newest snapshot is already normal. Repeated revisions do not restart
the window. Session/generation fencing scopes reset counters; a same-scope
decrease is treated conservatively as a changed history, never as recovery.

Ordinary local `/api/control-intent` requests also supply raw video health and
`critical_video_bad_sample_revision`/`critical_video_brake_revision`; missing
metadata requires a page refresh. Legal ESTOP remains independent of these
new page fields. Browser disconnect/end-session requests carry the expected
driver, session, generation, page owner, and ownership epoch so an old page
cannot release its replacement's session. Pending approvals additionally use
the expected connect cancellation generation.

The additive v1 envelope alone does not establish control compatibility.
Native V9 producers require `v9-revision-recovery-v1` from both the cloud and the
registered vehicle. Vehicle registration declares `native_control_capabilities`
for its connection generation; a missing declaration means unknown/legacy.
The native console sends `required_native_control_capabilities` when creating
each session, and the cloud verifies it before issuing control authority.
General session clients that omit this requirement retain the legacy API, but
cannot thereby make an old vehicle compatible with the new native console.
The intermediate local V8 candidate has no revision contract and must also be
rejected; its ability to carry a raw Boolean does not preserve coalesced faults.

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
