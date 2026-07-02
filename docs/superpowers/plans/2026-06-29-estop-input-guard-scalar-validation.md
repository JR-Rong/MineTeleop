# Estop Input Guard Scalar Validation

## Scope

Driver console design requires an intentional emergency-stop action, with a long-press guard before the command generator emits `estop=true`. The guard constructor previously accepted `True` as `1 ms` and leaked `TypeError` for string durations.

## RED

Command:

```bash
python3 -m unittest tests.test_design_behaviors.SignalingDriverAndClosedLoopTests.test_estop_input_guard_rejects_invalid_hold_duration
```

Result before implementation: failed because boolean durations were accepted, string durations raised `TypeError`, and zero used a less precise error message.

## GREEN

Changes:

- Added direct constructor validation for invalid emergency-stop hold durations.
- Required `required_hold_ms` to be a positive integer and rejected booleans explicitly.

Focused result after implementation: 1 test passed.

## Follow-up Verification

Run driver/closed-loop adjacent tests, compile checks, and `scripts/check.py`.
