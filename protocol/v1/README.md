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

## Draft machine-readable structural contract

`control-command.schema.json` is the draft JSON Schema (Draft 2020-12) for the
existing `control_command` wire object. It deliberately describes structure
only: required fields, JSON types, enum values, numeric ranges, and the v1
protocol version. It is not an authorization policy or a vehicle safety policy.

`fixtures/control-command-vectors.json` is the shared contract-vector manifest.
It reuses `control-command.valid.json` and
`control-command.invalid-missing-driver-id.json`, then adds missing-field,
wrong-type, empty-identity, sequence, range, gear, optional-estop, and unknown
field cases. The dedicated C++ and Node tests consume the same schema and
manifest.

Compatibility rules:

- The schema explicitly uses `"additionalProperties": true`. Unknown fields
  must be ignored by v1 consumers so additive fields remain compatible; they
  must not affect authorization, actuation, sequence handling, or safety
  decisions.
- `seq` is a positive JSON integer and `sent_at_utc_ms` is a non-negative JSON
  integer. Both are capped at `9007199254740991` (`Number.MAX_SAFE_INTEGER`),
  the portable C++/JavaScript representation intersection. C++ stores a wider
  `uint64_t`/`int64_t` range, but a JavaScript sender cannot safely represent
  every such value. A wider integer representation or a string representation
  requires a separately designed protocol version; it must not silently alter
  v1's wire type.
- `estop` is structurally optional in v1 and defaults to `false` at the current
  C++ wire boundary. Its safety effect remains runtime behavior.
- JSON Schema cannot authorize a control token, establish identity/session
  ownership, validate freshness, prevent replay or sequence reuse, clear an
  ESTOP, prove parking readiness, or validate non-JSON C++ values such as
  `NaN`/`Inf`. Existing state-machine and boundary validation remain required.

The C++ contract test requires every schema-valid vector to parse through the
current `ControlCommand::from_json` boundary and verifies that the shared
unknown-field vector is discarded on reserialization. It does not make the
schema a generated runtime validator: schema-invalid vectors are checked by the
cross-language structural test harness, while ingress authorization and timing
continue to be covered by their existing runtime tests. In particular, this R13
change does not claim that every current ingress independently rejects every
schema-invalid value; coupling the schema to all runtime parsers would require a
separate shared-core change.

Run the focused contract tests from a configured test build with:

```sh
ctest --test-dir build -R 'mine-teleop-protocol-v1-(contract|schema-js)-tests' --output-on-failure
```

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
