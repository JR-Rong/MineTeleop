# Driver Credential Validation

## Scope

Cloud service design allows loading PBKDF2-SHA256 driver credentials for deployment. Credential file loading already validates scalar fields; this slice hardens direct `DriverCredentialStore` construction so malformed credential objects fail authentication without leaking `TypeError`.

## RED

Command:

```bash
python3 -m unittest tests.test_design_behaviors.SignalingHttpServiceTests.test_identity_stores_reject_non_string_ids_tokens_and_boolean_ttl
```

Result before implementation: failed with `TypeError` when `DriverPasswordCredential.iterations` was a string.

## GREEN

Changes:

- `_verify_driver_password` now treats non-integer or boolean `iterations` as invalid credentials.
- The caller receives `PermissionError("invalid driver credentials")`, matching normal failed-login behavior.

Focused result after implementation: 1 test passed.

## Follow-up Verification

Run `SignalingHttpServiceTests`, compile checks, and `scripts/check.py`.
