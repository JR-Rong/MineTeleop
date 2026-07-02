from __future__ import annotations

import hashlib
import hmac
import http.client
import ipaddress
import json
import os
import shutil
import tempfile
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List
from urllib.parse import quote, urlencode, urlparse


UPLOAD_ITEM_STATUSES = frozenset({"pending", "uploading", "uploaded", "failed", "retry_wait", "credential_refresh"})


def _require_upload_item_string(item: dict, index: int, field: str) -> None:
    value = item.get(field)
    if not isinstance(value, str) or not value:
        raise ValueError(f"upload queue items[{index}].{field} must be a non-empty string")


def _optional_upload_item_string(item: dict, index: int, field: str, *, nullable: bool = False) -> None:
    if field not in item:
        return
    value = item[field]
    if nullable and value is None:
        return
    if not isinstance(value, str):
        suffix = "a string or null" if nullable else "a string"
        raise ValueError(f"upload queue items[{index}].{field} must be {suffix}")


def _required_upload_item_optional_ms(item: dict, index: int, field: str) -> None:
    if field not in item:
        raise ValueError(f"upload queue items[{index}].{field} must be a non-negative integer or null")
    _optional_upload_item_ms(item, index, field)


def _optional_upload_item_ms(item: dict, index: int, field: str) -> None:
    if field not in item:
        return
    value = item[field]
    if value is not None and (isinstance(value, bool) or not isinstance(value, int) or value < 0):
        raise ValueError(f"upload queue items[{index}].{field} must be a non-negative integer or null")


def _validate_upload_item_payload(item: dict, index: int) -> None:
    for field in ("segment_id", "video_path", "metadata_path", "object_path"):
        _require_upload_item_string(item, index, field)
    _optional_upload_item_string(item, index, "upload_url", nullable=True)
    _required_upload_item_optional_ms(item, index, "expires_at_ms")
    for field in ("metadata_object_path", "video_sha256", "metadata_sha256"):
        _optional_upload_item_string(item, index, field)
    for field in ("metadata_upload_url", "last_error"):
        _optional_upload_item_string(item, index, field, nullable=True)
    for field in ("metadata_expires_at_ms", "enqueued_at_ms", "next_retry_at_ms"):
        _optional_upload_item_ms(item, index, field)
    retry_count = item.get("retry_count", 0)
    if isinstance(retry_count, bool) or not isinstance(retry_count, int) or retry_count < 0:
        raise ValueError(f"upload queue items[{index}].retry_count must be a non-negative integer")
    status = item.get("status", "pending")
    if not isinstance(status, str) or status not in UPLOAD_ITEM_STATUSES:
        expected = ", ".join(sorted(UPLOAD_ITEM_STATUSES))
        raise ValueError(f"upload queue items[{index}].status must be one of {expected}")


@dataclass
class UploadItem:
    segment_id: str
    video_path: str
    metadata_path: str
    object_path: str
    upload_url: str | None
    expires_at_ms: int | None
    metadata_object_path: str = ""
    metadata_upload_url: str | None = None
    metadata_expires_at_ms: int | None = None
    video_sha256: str = ""
    metadata_sha256: str = ""
    enqueued_at_ms: int | None = None
    status: str = "pending"
    retry_count: int = 0
    last_error: str | None = None
    next_retry_at_ms: int | None = None


@dataclass(frozen=True)
class UploadAction:
    action: str
    item: UploadItem


@dataclass(frozen=True)
class UploadDispatchDecision:
    dispatch: bool
    reason: str


@dataclass(frozen=True)
class NetworkQualitySample:
    connected: bool
    rtt_ms: int
    jitter_ms: int
    packet_loss_percent: float
    uplink_mbps: float


@dataclass(frozen=True)
class UploadPauseDecision:
    pause: bool
    reason: str


class UploadNetworkQualityPolicy:
    def __init__(
        self,
        max_rtt_ms: int,
        max_jitter_ms: int,
        max_loss_percent: float,
        min_uplink_mbps: float,
    ) -> None:
        if (
            _invalid_int(max_rtt_ms, minimum=1)
            or _invalid_int(max_jitter_ms, minimum=0)
            or _invalid_number(max_loss_percent, minimum=0)
            or _invalid_number(min_uplink_mbps, minimum=0, allow_zero=False)
        ):
            raise ValueError("network quality thresholds must be non-negative and bandwidth must be positive")
        self.max_rtt_ms = max_rtt_ms
        self.max_jitter_ms = max_jitter_ms
        self.max_loss_percent = max_loss_percent
        self.min_uplink_mbps = min_uplink_mbps

    def evaluate(self, sample: NetworkQualitySample) -> UploadPauseDecision:
        if not isinstance(sample.connected, bool):
            raise ValueError("network quality sample connected must be a boolean")
        if _invalid_int(sample.rtt_ms, minimum=0):
            raise ValueError("network quality sample rtt_ms must be a non-negative integer")
        if _invalid_int(sample.jitter_ms, minimum=0):
            raise ValueError("network quality sample jitter_ms must be a non-negative integer")
        if _invalid_number(sample.packet_loss_percent, minimum=0):
            raise ValueError("network quality sample packet_loss_percent must be non-negative")
        if _invalid_number(sample.uplink_mbps, minimum=0):
            raise ValueError("network quality sample uplink_mbps must be non-negative")
        if not sample.connected:
            return UploadPauseDecision(True, "network_disconnected")
        if sample.rtt_ms > self.max_rtt_ms:
            return UploadPauseDecision(True, "network_rtt_exceeded")
        if sample.jitter_ms > self.max_jitter_ms:
            return UploadPauseDecision(True, "network_jitter_exceeded")
        if sample.packet_loss_percent > self.max_loss_percent:
            return UploadPauseDecision(True, "network_loss_exceeded")
        if sample.uplink_mbps < self.min_uplink_mbps:
            return UploadPauseDecision(True, "network_uplink_below_minimum")
        return UploadPauseDecision(False, "network_quality_ok")


def _invalid_int(value: object, minimum: int) -> bool:
    return isinstance(value, bool) or not isinstance(value, int) or value < minimum


def _invalid_number(value: object, minimum: float, allow_zero: bool = True) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return True
    if allow_zero:
        return value < minimum
    return value <= minimum


def _required_non_empty_string(value: object, field: str, *, empty_message: str | None = None) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a non-empty string")
    if not value.strip():
        raise ValueError(empty_message or f"{field} must be a non-empty string")
    return value


def _optional_string(value: object, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a string")
    return value


class UploadBandwidthLimiter:
    def __init__(self, max_mbps: float) -> None:
        if _invalid_number(max_mbps, minimum=0, allow_zero=False):
            raise ValueError("max_mbps must be positive")
        self.max_mbps = max_mbps
        self._next_allowed_ms = 0

    def retry_after_ms(self, now_ms: int) -> int:
        return max(0, self._next_allowed_ms - now_ms)

    def record_upload(self, bytes_uploaded: int, finished_at_ms: int) -> None:
        if isinstance(bytes_uploaded, bool) or not isinstance(bytes_uploaded, int):
            raise ValueError("bytes_uploaded must be a non-negative integer")
        if bytes_uploaded < 0:
            raise ValueError("bytes_uploaded must be non-negative")
        if isinstance(finished_at_ms, bool) or not isinstance(finished_at_ms, int):
            raise ValueError("finished_at_ms must be a non-negative integer")
        if finished_at_ms < 0:
            raise ValueError("finished_at_ms must be non-negative")
        bits_per_ms = self.max_mbps * 1_000_000 / 1000
        interval_ms = int((bytes_uploaded * 8 + bits_per_ms - 1) // bits_per_ms)
        self._next_allowed_ms = finished_at_ms + max(0, interval_ms)


@dataclass(frozen=True)
class UploadTriggerPolicy:
    trigger_segments: int = 1
    trigger_bytes: int | None = None
    trigger_interval_ms: int | None = None
    network_idle_enabled: bool = False

    def __post_init__(self) -> None:
        if _invalid_int(self.trigger_segments, minimum=1):
            raise ValueError("trigger_segments must be a positive integer")
        if self.trigger_bytes is not None and _invalid_int(self.trigger_bytes, minimum=1):
            raise ValueError("trigger_bytes must be a positive integer or null")
        if self.trigger_interval_ms is not None and _invalid_int(self.trigger_interval_ms, minimum=1):
            raise ValueError("trigger_interval_ms must be a positive integer or null")
        if not isinstance(self.network_idle_enabled, bool):
            raise ValueError("network_idle_enabled must be a boolean")

    def evaluate(
        self,
        pending_segments: int,
        pending_bytes: int,
        oldest_pending_age_ms: int,
        network_idle: bool = False,
    ) -> UploadDispatchDecision:
        if _invalid_int(pending_segments, minimum=0):
            raise ValueError("pending_segments must be a non-negative integer")
        if _invalid_int(pending_bytes, minimum=0):
            raise ValueError("pending_bytes must be a non-negative integer")
        if _invalid_int(oldest_pending_age_ms, minimum=0):
            raise ValueError("oldest_pending_age_ms must be a non-negative integer")
        if not isinstance(network_idle, bool):
            raise ValueError("network_idle must be a boolean")
        if pending_segments <= 0:
            return UploadDispatchDecision(False, "no_pending_segments")
        if self.network_idle_enabled and network_idle:
            return UploadDispatchDecision(True, "network_idle")
        if pending_segments >= self.trigger_segments:
            return UploadDispatchDecision(True, "segment_count")
        if self.trigger_bytes is not None and pending_bytes >= self.trigger_bytes:
            return UploadDispatchDecision(True, "accumulated_bytes")
        if self.trigger_interval_ms is not None and oldest_pending_age_ms >= self.trigger_interval_ms:
            return UploadDispatchDecision(True, "time_window")
        return UploadDispatchDecision(False, "waiting_for_trigger")


class UploadQueue:
    def __init__(
        self,
        state_path: Path | str,
        refresh_margin_seconds: int,
        retry_initial_seconds: int = 10,
        retry_max_seconds: int = 600,
    ) -> None:
        if _invalid_int(refresh_margin_seconds, minimum=1):
            raise ValueError("refresh_margin_seconds must be positive")
        if _invalid_int(retry_initial_seconds, minimum=1):
            raise ValueError("retry_initial_seconds must be positive")
        if _invalid_int(retry_max_seconds, minimum=1):
            raise ValueError("retry_max_seconds must be positive")
        if retry_initial_seconds > retry_max_seconds:
            raise ValueError("retry_initial_seconds must be less than or equal to retry_max_seconds")
        self.state_path = Path(state_path)
        self.refresh_margin_ms = refresh_margin_seconds * 1000
        self.retry_initial_ms = retry_initial_seconds * 1000
        self.retry_max_ms = retry_max_seconds * 1000
        self.items: List[UploadItem] = []
        self.paused = False
        self.pause_reason: str | None = None
        self._load()

    def enqueue(
        self,
        segment_id: str,
        video_path: str,
        metadata_path: str,
        object_path: str,
        upload_url: str | None,
        expires_at_ms: int | None,
        metadata_object_path: str = "",
        metadata_upload_url: str | None = None,
        metadata_expires_at_ms: int | None = None,
        video_sha256: str = "",
        metadata_sha256: str = "",
        enqueued_at_ms: int | None = None,
    ) -> None:
        item = UploadItem(
            segment_id=segment_id,
            video_path=video_path,
            metadata_path=metadata_path,
            object_path=object_path,
            upload_url=upload_url,
            expires_at_ms=expires_at_ms,
            metadata_object_path=metadata_object_path,
            metadata_upload_url=metadata_upload_url,
            metadata_expires_at_ms=metadata_expires_at_ms,
            video_sha256=video_sha256,
            metadata_sha256=metadata_sha256,
            enqueued_at_ms=enqueued_at_ms if enqueued_at_ms is not None else int(time.time() * 1000),
        )
        _validate_upload_item_payload(asdict(item), 0)
        self.items.append(item)
        self._save()

    def next_action(self, now_ms: int) -> UploadAction:
        if self.paused:
            return UploadAction("paused", self._first_actionable_item())
        for item in self.items:
            if item.status in {"pending", "retry_wait", "credential_refresh"}:
                if item.status == "retry_wait" and item.next_retry_at_ms is not None and now_ms < item.next_retry_at_ms:
                    return UploadAction("wait", item)
                expires_at = [
                    value
                    for value in (item.expires_at_ms, item.metadata_expires_at_ms)
                    if value is not None
                ]
                if any(value - now_ms <= self.refresh_margin_ms for value in expires_at):
                    item.status = "credential_refresh"
                    self._save()
                    return UploadAction("credential_refresh", item)
                item.status = "uploading"
                self._save()
                return UploadAction("upload", item)
        raise IndexError("upload queue has no actionable items")

    def pause(self, reason: str) -> None:
        if not isinstance(reason, str) or not reason:
            raise ValueError("upload queue pause reason must be a non-empty string")
        self.paused = True
        self.pause_reason = reason
        self._save()

    def resume(self) -> None:
        self.paused = False
        self.pause_reason = None
        self._save()

    def mark_uploaded(self, segment_id: str) -> None:
        for item in self.items:
            if item.segment_id == segment_id:
                item.status = "uploaded"
                item.last_error = None
                item.next_retry_at_ms = None
                self._save()
                return
        raise KeyError(segment_id)

    def mark_failed(self, segment_id: str, error: str, now_ms: int) -> None:
        if not isinstance(error, str) or not error:
            raise ValueError("upload queue failure reason must be a non-empty string")
        for item in self.items:
            if item.segment_id == segment_id:
                item.retry_count += 1
                delay_ms = min(self.retry_initial_ms * (2 ** (item.retry_count - 1)), self.retry_max_ms)
                item.status = "retry_wait"
                item.last_error = error
                item.next_retry_at_ms = now_ms + delay_ms
                self._save()
                return
        raise KeyError(segment_id)

    def _load(self) -> None:
        if not self.state_path.exists():
            return
        raw = json.loads(self.state_path.read_text(encoding="utf-8"))
        paused = raw.get("paused", False)
        if not isinstance(paused, bool):
            raise ValueError("upload queue paused must be a boolean")
        pause_reason = raw.get("pause_reason")
        if pause_reason is not None and not isinstance(pause_reason, str):
            raise ValueError("upload queue pause_reason must be a string or null")
        items = raw.get("items", [])
        if not isinstance(items, list):
            raise ValueError("upload queue items must be a list")
        for index, item in enumerate(items):
            if not isinstance(item, dict):
                raise ValueError(f"upload queue items[{index}] must be an object")
            _validate_upload_item_payload(item, index)
        self.paused = paused
        self.pause_reason = pause_reason
        self.items = [UploadItem(**item) for item in items]

    def _save(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "paused": self.paused,
            "pause_reason": self.pause_reason,
            "items": [asdict(item) for item in self.items],
        }
        text = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True)
        # Write atomically so a crash/power loss (common in field vehicles) cannot
        # leave a truncated/corrupt queue. The queue persists presigned upload URLs
        # (bearer credentials) at rest, so restrict permissions.
        tmp_path = self.state_path.with_name(self.state_path.name + ".tmp")
        tmp_path.write_text(text, encoding="utf-8")
        try:
            tmp_path.chmod(0o600)
        except OSError:
            pass
        tmp_path.replace(self.state_path)

    def _first_actionable_item(self) -> UploadItem:
        for item in self.items:
            if item.status in {"pending", "retry_wait", "credential_refresh"}:
                return item
        raise IndexError("upload queue has no actionable items")


@dataclass(frozen=True)
class UploadResult:
    segment_id: str
    status: str
    video_object_path: Path | None
    metadata_object_path: Path | None


class LocalArchiveUploader:
    def __init__(self, root_dir: Path | str) -> None:
        self.root_dir = Path(root_dir)

    def upload(
        self,
        segment_id: str,
        video_path: Path | str,
        metadata_path: Path | str,
        object_path: str,
        metadata_object_path: str = "",
        upload_url: str | None = None,
        metadata_upload_url: str | None = None,
    ) -> UploadResult:
        video_destination = _archive_destination(self.root_dir, object_path)
        metadata_destination = (
            _archive_destination(self.root_dir, metadata_object_path)
            if metadata_object_path
            else video_destination.with_suffix(".json")
        )
        video_destination.parent.mkdir(parents=True, exist_ok=True)
        metadata_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(video_path, video_destination)
        shutil.copy2(metadata_path, metadata_destination)
        return UploadResult(
            segment_id=segment_id,
            status="uploaded",
            video_object_path=video_destination,
            metadata_object_path=metadata_destination,
        )


class HttpPutUploader:
    def __init__(self, timeout_seconds: float = 30.0) -> None:
        if _invalid_number(timeout_seconds, minimum=0, allow_zero=False):
            raise ValueError("timeout_seconds must be positive")
        self.timeout_seconds = timeout_seconds

    def upload(
        self,
        segment_id: str,
        video_path: Path | str,
        metadata_path: Path | str,
        object_path: str,
        metadata_object_path: str = "",
        upload_url: str | None = None,
        metadata_upload_url: str | None = None,
    ) -> UploadResult:
        if not upload_url:
            raise ValueError("upload_url is required for direct HTTP PUT uploads")
        if not metadata_upload_url:
            raise ValueError("metadata_upload_url is required for direct HTTP PUT uploads")
        self._put_file(upload_url, Path(video_path))
        with self._uploaded_metadata_copy(Path(metadata_path)) as upload_metadata_path:
            self._put_file(metadata_upload_url, upload_metadata_path)
        return UploadResult(
            segment_id=segment_id,
            status="uploaded",
            video_object_path=None,
            metadata_object_path=None,
        )

    @contextmanager
    def _uploaded_metadata_copy(self, metadata_path: Path):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if isinstance(metadata, dict):
            metadata["upload_state"] = "uploaded"
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False) as handle:
            temp_path = Path(handle.name)
            json.dump(metadata, handle, ensure_ascii=False, sort_keys=True)
        try:
            yield temp_path
        finally:
            temp_path.unlink(missing_ok=True)

    def _put_file(self, upload_url: str, source_path: Path) -> None:
        parsed = urlparse(upload_url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise ValueError("upload_url must include http(s) scheme and host")
        if parsed.scheme == "http" and not _is_loopback_netloc(parsed.netloc):
            raise ValueError("refusing to PUT upload over plaintext http to a non-loopback host")
        target = parsed.path or "/"
        if parsed.query:
            target = f"{target}?{parsed.query}"
        connection_class = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        connection = connection_class(parsed.netloc, timeout=self.timeout_seconds)
        try:
            with source_path.open("rb") as body:
                # Take the length from the open handle to avoid a TOCTOU size
                # mismatch if the file is rotated between stat() and send.
                content_length = os.fstat(body.fileno()).st_size
                connection.request(
                    "PUT",
                    target,
                    body=body,
                    headers={
                        "Content-Length": str(content_length),
                        "Content-Type": "application/octet-stream",
                    },
                )
                response = connection.getresponse()
                response.read()
            if response.status < 200 or response.status >= 300:
                raise RuntimeError(f"direct HTTP PUT failed with status {response.status}")
        finally:
            connection.close()


@dataclass(frozen=True)
class UploadCredential:
    segment_id: str
    kind: str
    object_path: str
    upload_url: str
    issued_at_ms: int
    expires_at_ms: int


@dataclass(frozen=True)
class UploadRecord:
    segment_id: str
    object_path: str
    status: str
    bytes_uploaded: int = 0
    error: str = ""


@dataclass(frozen=True)
class S3PresignConfig:
    endpoint_url: str
    bucket: str
    region: str
    access_key_id: str
    secret_access_key: str
    session_token: str = ""


class S3PresignedPutSigner:
    def __init__(self, config: S3PresignConfig) -> None:
        self.config = config

    def presign_put(self, object_path: str, expires_seconds: int, now_ms: int) -> str:
        if isinstance(expires_seconds, bool) or not isinstance(expires_seconds, int) or expires_seconds <= 0:
            raise ValueError("expires_seconds must be a positive integer")
        if not _is_safe_relative_object_path(object_path):
            raise ValueError("object_path must contain safe relative path segments")
        endpoint_url = _required_non_empty_string(self.config.endpoint_url, "endpoint_url")
        bucket = _required_non_empty_string(self.config.bucket, "bucket", empty_message="bucket is required")
        region = _required_non_empty_string(self.config.region, "region", empty_message="region is required")
        access_key_id = _required_non_empty_string(
            self.config.access_key_id,
            "access_key_id",
            empty_message="access_key_id is required",
        )
        secret_access_key = _required_non_empty_string(
            self.config.secret_access_key,
            "secret_access_key",
            empty_message="secret_access_key is required",
        )
        session_token = _optional_string(self.config.session_token, "session_token")
        timestamp = datetime.fromtimestamp(now_ms / 1000, tz=timezone.utc)
        amz_date = timestamp.strftime("%Y%m%dT%H%M%SZ")
        date_scope = timestamp.strftime("%Y%m%d")
        endpoint = urlparse(endpoint_url.rstrip("/"))
        if endpoint.scheme not in {"http", "https"} or not endpoint.netloc:
            raise ValueError("endpoint_url must include http(s) scheme and host")
        canonical_uri = "/" + "/".join(
            quote(part, safe="") for part in [bucket, *object_path.split("/")]
        )
        credential_scope = f"{date_scope}/{region}/s3/aws4_request"
        query = {
            "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
            "X-Amz-Content-Sha256": "UNSIGNED-PAYLOAD",
            "X-Amz-Credential": f"{access_key_id}/{credential_scope}",
            "X-Amz-Date": amz_date,
            "X-Amz-Expires": str(expires_seconds),
            "X-Amz-SignedHeaders": "host",
        }
        if session_token:
            query["X-Amz-Security-Token"] = session_token
        canonical_query = urlencode(sorted(query.items()), quote_via=quote, safe="")
        canonical_headers = f"host:{endpoint.netloc}\n"
        canonical_request = "\n".join(
            [
                "PUT",
                canonical_uri,
                canonical_query,
                canonical_headers,
                "host",
                "UNSIGNED-PAYLOAD",
            ]
        )
        string_to_sign = "\n".join(
            [
                "AWS4-HMAC-SHA256",
                amz_date,
                credential_scope,
                hashlib.sha256(canonical_request.encode("utf-8")).hexdigest(),
            ]
        )
        signature = hmac.new(
            self._signing_key(date_scope, secret_access_key, region),
            string_to_sign.encode("utf-8"),
            hashlib.sha256,
        ).hexdigest()
        signed_query = canonical_query + f"&X-Amz-Signature={signature}"
        return f"{endpoint.scheme}://{endpoint.netloc}{canonical_uri}?{signed_query}"

    def _signing_key(self, date_scope: str, secret_access_key: str, region: str) -> bytes:
        date_key = _hmac_sha256(("AWS4" + secret_access_key).encode("utf-8"), date_scope)
        region_key = _hmac_sha256(date_key, region)
        service_key = _hmac_sha256(region_key, "s3")
        return _hmac_sha256(service_key, "aws4_request")


class UploadCredentialService:
    def __init__(
        self,
        public_base_url: str = "http://127.0.0.1:0",
        ttl_seconds: int = 900,
        s3_signer: S3PresignedPutSigner | None = None,
    ) -> None:
        if isinstance(ttl_seconds, bool) or not isinstance(ttl_seconds, int) or ttl_seconds <= 0:
            raise ValueError("ttl_seconds must be a positive integer")
        self.public_base_url = public_base_url.rstrip("/")
        self.ttl_ms = ttl_seconds * 1000
        self.ttl_seconds = ttl_seconds
        self.s3_signer = s3_signer
        self._issue_counter = 0
        self.records: Dict[str, UploadRecord] = {}

    def issue(self, request: Dict[str, str], now_ms: int | None = None) -> UploadCredential:
        now = now_ms if now_ms is not None else int(time.time() * 1000)
        segment_id = _required(request, "segment_id")
        kind = _required_string(request, "kind")
        object_path = self._object_path(request)
        self._issue_counter += 1
        if self.s3_signer is None:
            upload_url = f"{self.public_base_url}/upload-target/{self._issue_counter:06d}/{object_path}"
        else:
            upload_url = self.s3_signer.presign_put(
                object_path=object_path,
                expires_seconds=self.ttl_seconds,
                now_ms=now,
            )
        return UploadCredential(
            segment_id=segment_id,
            kind=kind,
            object_path=object_path,
            upload_url=upload_url,
            issued_at_ms=now,
            expires_at_ms=now + self.ttl_ms,
        )

    def mark_uploaded(self, segment_id: str, object_path: str, bytes_uploaded: int) -> UploadRecord:
        segment_id = _required_string({"segment_id": segment_id}, "segment_id")
        object_path = _required_string({"object_path": object_path}, "object_path")
        if isinstance(bytes_uploaded, bool) or not isinstance(bytes_uploaded, int):
            raise ValueError("bytes_uploaded must be a non-negative integer")
        if bytes_uploaded < 0:
            raise ValueError("bytes_uploaded must be non-negative")
        record = UploadRecord(
            segment_id=segment_id,
            object_path=object_path,
            status="uploaded",
            bytes_uploaded=bytes_uploaded,
        )
        self.records[object_path] = record
        return record

    def mark_failed(self, segment_id: str, object_path: str, error: str) -> UploadRecord:
        segment_id = _required_string({"segment_id": segment_id}, "segment_id")
        object_path = _required_string({"object_path": object_path}, "object_path")
        if not isinstance(error, str):
            raise ValueError("upload failure error must be a non-empty string")
        if not error.strip():
            raise ValueError("upload failure error is required")
        previous = self.records.get(object_path)
        record = replace(previous, status="failed", error=error) if previous else UploadRecord(
            segment_id=segment_id,
            object_path=object_path,
            status="failed",
            error=error,
        )
        self.records[object_path] = record
        return record

    def _object_path(self, request: Dict[str, str]) -> str:
        vehicle_id = _required_path_segment(request, "vehicle_id")
        session_id = _required_path_segment(request, "session_id")
        segment_id = _required_path_segment(request, "segment_id")
        kind = _required_string(request, "kind")
        if kind == "video":
            camera_id = _required_path_segment(request, "camera_id")
            return f"vehicles/{vehicle_id}/sessions/{session_id}/cameras/{camera_id}/{segment_id}.mp4"
        if kind == "metadata":
            return f"vehicles/{vehicle_id}/sessions/{session_id}/metadata/{segment_id}.json"
        raise ValueError("kind must be video or metadata")


def upload_credential_service_from_config(
    upload_config: object,
    public_base_url: str = "http://127.0.0.1:0",
    ttl_seconds: int = 900,
) -> UploadCredentialService:
    if getattr(upload_config, "backend") != "s3":
        return UploadCredentialService(public_base_url=public_base_url, ttl_seconds=ttl_seconds)
    s3 = getattr(upload_config, "s3", None)
    if s3 is None:
        raise ValueError("upload.s3 is required for s3 backend")
    signer = S3PresignedPutSigner(
        S3PresignConfig(
            endpoint_url=s3.endpoint_url,
            bucket=s3.bucket,
            region=s3.region,
            access_key_id=s3.access_key_id,
            secret_access_key=_secret_value(
                value=s3.secret_access_key,
                file_path=s3.secret_access_key_file,
                label="upload.s3.secret_access_key",
            ),
            session_token=_secret_value(
                value=s3.session_token,
                file_path=s3.session_token_file,
                label="upload.s3.session_token",
                required=False,
            ),
        )
    )
    return UploadCredentialService(
        public_base_url=public_base_url,
        ttl_seconds=ttl_seconds,
        s3_signer=signer,
    )


def _required(payload: Dict[str, str], key: str) -> str:
    value = str(payload.get(key, ""))
    if not value:
        raise ValueError(f"{key} is required")
    return value


def _required_string(payload: Dict[str, str], key: str) -> str:
    value = payload.get(key, "")
    if not isinstance(value, str) or not value:
        raise ValueError(f"{key} must be a non-empty string")
    return value


def _required_path_segment(payload: Dict[str, str], key: str) -> str:
    value = payload.get(key, "")
    if not isinstance(value, str) or not value or value in {".", ".."} or "/" in value or "\\" in value:
        raise ValueError(f"{key} must be a safe object path segment")
    return value


def _archive_destination(root_dir: Path, object_path: str) -> Path:
    if not object_path or Path(object_path).is_absolute():
        raise ValueError("object_path must stay under archive root")
    root = root_dir.resolve()
    destination = (root_dir / object_path).resolve()
    try:
        destination.relative_to(root)
    except ValueError as exc:
        raise ValueError("object_path must stay under archive root") from exc
    return destination


def _is_safe_relative_object_path(object_path: str) -> bool:
    if not object_path or object_path.startswith("/") or "\\" in object_path:
        return False
    parts = object_path.split("/")
    return all(part and part not in {".", ".."} for part in parts)


def _secret_value(value: str | None, file_path: str | None, label: str, required: bool = True) -> str:
    # An empty/whitespace inline value must not silently win over a configured
    # secret file; treat it as unset and fall through.
    if value is not None and value.strip():
        return value
    if file_path is not None:
        try:
            secret = Path(file_path).read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise ValueError(f"{label} secret file could not be read: {exc}") from exc
        if secret:
            return secret
    if required:
        raise ValueError(f"{label} is required")
    return ""


def _hmac_sha256(key: bytes, value: str) -> bytes:
    return hmac.new(key, value.encode("utf-8"), hashlib.sha256).digest()


def _is_loopback_netloc(netloc: str) -> bool:
    host = netloc.rsplit("@", 1)[-1]
    if host.startswith("["):  # IPv6 literal, e.g. [::1]:8443
        host = host[1:].split("]", 1)[0]
    else:
        host = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False
