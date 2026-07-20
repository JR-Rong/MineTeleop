# Upload Scalar Validation Slice

## Scope

Design docs require upload and S3 configuration failures to be rejected clearly before runtime dispatch. This slice covers direct API construction paths that could previously leak `TypeError` or `AttributeError` when non-numeric upload limits, upload queue timing values, or non-string S3 signing fields were supplied.

## RED

Command:

```bash
python3 -m unittest tests.test_design_behaviors.MediaRecordingUploadTests.test_upload_bandwidth_limiter_rejects_boolean_limit tests.test_design_behaviors.MediaRecordingUploadTests.test_http_put_uploader_rejects_invalid_timeout_types tests.test_design_behaviors.MediaRecordingUploadTests.test_s3_presigner_rejects_non_string_signing_fields
```

Result before implementation: failed with `TypeError` for string numeric limits, `AttributeError` for non-string S3 fields, and no error for boolean `session_token`.

Additional RED:

```bash
python3 -m unittest tests.test_design_behaviors.MediaRecordingUploadTests.test_upload_queue_rejects_invalid_timing_settings
```

Result before implementation: failed with `TypeError` for string upload queue timing values.

## GREEN

Changes:

- Added explicit tests for non-numeric upload bandwidth and HTTP timeout values.
- Added explicit tests for non-integer upload queue timing values.
- Added explicit tests for non-string S3 presign config fields.
- Reused numeric validation and added small string validation helpers in `mine_teleop.upload`.
- Preserved existing empty-field messages for required S3 signing fields.

Focused result after implementation: 3 presign/limit tests passed; upload queue timing settings test passed.

## Follow-up Verification

Run the media upload test class, config/upload adjacent tests, `py_compile`, `git diff --check`, and `scripts/check.py`.
