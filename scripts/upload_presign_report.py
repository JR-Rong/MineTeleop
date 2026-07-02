#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import load_vehicle_config  # noqa: E402
from mine_teleop.upload import upload_credential_service_from_config  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit a redacted S3 upload presign acceptance report.")
    parser.add_argument("--vehicle-config", required=True)
    parser.add_argument("--vehicle-id", required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--camera-id", required=True)
    parser.add_argument("--segment-id", required=True)
    parser.add_argument("--ttl-seconds", type=int, default=900)
    parser.add_argument("--now-ms", type=int, default=0)
    args = parser.parse_args()

    config = load_vehicle_config(args.vehicle_config)
    if config.upload.backend != "s3" or config.upload.s3 is None:
        parser.error("upload.backend must be s3 and upload.s3 must be configured")

    service = upload_credential_service_from_config(config.upload, ttl_seconds=args.ttl_seconds)
    requests = (
        {
            "vehicle_id": args.vehicle_id,
            "session_id": args.session_id,
            "camera_id": args.camera_id,
            "segment_id": args.segment_id,
            "kind": "video",
        },
        {
            "vehicle_id": args.vehicle_id,
            "session_id": args.session_id,
            "segment_id": args.segment_id,
            "kind": "metadata",
        },
    )
    credentials = [service.issue(request, now_ms=args.now_ms) for request in requests]
    endpoint = urlparse(config.upload.s3.endpoint_url.rstrip("/"))
    summary = {
        "event": "s3_upload_presign_report",
        "passed": True,
        "backend": "s3",
        "credential_count": len(credentials),
        "endpoint_host": endpoint.netloc,
        "bucket": config.upload.s3.bucket,
        "region": config.upload.s3.region,
    }
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    for credential in credentials:
        print(json.dumps(_credential_record(credential), ensure_ascii=False, sort_keys=True))
    return 0


def _credential_record(credential) -> dict[str, object]:
    parsed = urlparse(credential.upload_url)
    query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
    return {
        "event": "s3_upload_presign_credential",
        "kind": credential.kind,
        "object_path": credential.object_path,
        "url_scheme": parsed.scheme,
        "url_host": parsed.netloc,
        "algorithm": query.get("X-Amz-Algorithm", ""),
        "content_sha256": query.get("X-Amz-Content-Sha256", ""),
        "credential_scope": _redacted_credential_scope(query.get("X-Amz-Credential", "")),
        "expires_seconds": _int_or_none(query.get("X-Amz-Expires")),
        "signed_headers": query.get("X-Amz-SignedHeaders", ""),
        "signature_present": bool(query.get("X-Amz-Signature")),
        "session_token_present": "X-Amz-Security-Token" in query,
        "issued_at_ms": credential.issued_at_ms,
        "expires_at_ms": credential.expires_at_ms,
    }


def _redacted_credential_scope(value: str) -> str:
    if "/" not in value:
        return "configured" if value else ""
    return "configured/" + value.split("/", 1)[1]


def _int_or_none(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


if __name__ == "__main__":
    raise SystemExit(main())
