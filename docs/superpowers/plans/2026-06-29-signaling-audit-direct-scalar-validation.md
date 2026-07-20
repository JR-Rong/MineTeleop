# Signaling Audit Direct Scalar Validation

## Scope

Design docs require cloud audit endpoints to reject malformed metrics and avoid writing misleading audit records. HTTP handlers already validate JSON payload fields before calling service methods; this slice hardens the direct `SignalingHttpService` audit API so internal callers cannot pass booleans or strings that are accepted as metrics or leak raw `TypeError`.

## RED

Command:

```bash
python3 -m unittest tests.test_design_behaviors.SignalingHttpServiceTests.test_service_audit_methods_reject_direct_wrong_scalar_types
```

Result before implementation: failed because boolean `rtt_ms` and `bytes_sent` were accepted and written to the audit log, while string numeric values raised raw `TypeError`.

## GREEN

Changes:

- Added value-level non-negative integer and number validators in `mine_teleop.signaling_service`.
- Applied validators to realtime diagnostics, control timeout, estop sequence, TURN usage, and trusted coturn usage direct APIs.
- Preserved HTTP route validation behavior while preventing direct bad calls from writing audit events.

Focused result after implementation: 1 test passed.

## Follow-up Verification

Run the full `SignalingHttpServiceTests` class, adjacent compile checks, and `scripts/check.py`.
