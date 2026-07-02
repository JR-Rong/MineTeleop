#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ipaddress
import json
import ssl
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import effective_vehicle_config_log_payload, load_vehicle_config
from mine_teleop.signaling import SessionManager
from mine_teleop.signaling_service import DeviceCredentialStore, DriverCredentialStore, SignalingHttpService
from mine_teleop.upload import upload_credential_service_from_config


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Mine Teleop signaling-server development entrypoint.")
    parser.add_argument("--serve", action="store_true", help="Start the local HTTP JSON signaling service.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--port-file", default="")
    parser.add_argument("--audit-log", default=".local/signaling-audit.jsonl")
    parser.add_argument("--audit-log-max-bytes", type=int, help="Rotate the audit log before this size.")
    parser.add_argument("--audit-log-backup-count", type=int, default=0, help="Number of rotated audit logs to keep.")
    parser.add_argument("--vehicle-config", default="", help="Optional vehicle config used for upload credential signing.")
    parser.add_argument("--driver-credentials", default="", help="Optional JSON file with PBKDF2 driver password credentials.")
    parser.add_argument("--device-credentials", default="", help="Optional JSON file with vehicle device tokens.")
    parser.add_argument("--upload-public-base-url", default="", help="Public base URL for local placeholder upload URLs.")
    parser.add_argument("--tls-cert", default="", help="TLS certificate chain for non-loopback HTTPS serving.")
    parser.add_argument("--tls-key", default="", help="TLS private key for non-loopback HTTPS serving.")
    args = parser.parse_args()

    if args.serve:
        tls_enabled = bool(args.tls_cert or args.tls_key)
        if bool(args.tls_cert) != bool(args.tls_key):
            parser.error("--tls-cert and --tls-key must be configured together")
        if not _is_loopback_host(args.host) and not tls_enabled:
            parser.error("--tls-cert and --tls-key are required for non-loopback hosts")
        # Fail closed: never fall back to the built-in dev credentials on a
        # network-reachable deployment. Loopback (dev/test) may still use them.
        if not _is_loopback_host(args.host):
            if not args.driver_credentials:
                parser.error("--driver-credentials is required for non-loopback hosts")
            if not args.device_credentials:
                parser.error("--device-credentials is required for non-loopback hosts")

        upload_credentials = None
        ice_config = None
        driver_credentials = None
        if args.driver_credentials:
            driver_credentials = DriverCredentialStore.from_json_file(args.driver_credentials)
        device_credentials = None
        if args.device_credentials:
            device_credentials = DeviceCredentialStore.from_json_file(args.device_credentials)
        if args.vehicle_config:
            vehicle_config = load_vehicle_config(args.vehicle_config)
            print(json.dumps(effective_vehicle_config_log_payload(vehicle_config), ensure_ascii=False, sort_keys=True))
            ice_config = vehicle_config.ice
            upload_credentials = upload_credential_service_from_config(
                vehicle_config.upload,
                public_base_url=args.upload_public_base_url or "http://127.0.0.1:0",
            )
        service = SignalingHttpService(
            audit_log_path=args.audit_log,
            upload_credentials=upload_credentials,
            ice_config=ice_config,
            driver_credentials=driver_credentials,
            device_credentials=device_credentials,
            audit_log_max_bytes=args.audit_log_max_bytes,
            audit_log_backup_count=args.audit_log_backup_count,
        )
        server = service.make_server(args.host, args.port)
        if tls_enabled:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(args.tls_cert, args.tls_key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
        actual_host, actual_port = server.server_address
        if args.port_file:
            Path(args.port_file).write_text(str(actual_port), encoding="utf-8")
        scheme = "https" if tls_enabled else "http"
        print(
            json.dumps(
                {"event": "signaling_server_startup", "host": actual_host, "port": actual_port, "scheme": scheme, "status": "serving"},
                ensure_ascii=False,
                sort_keys=True,
            ),
            flush=True,
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
        return 0

    manager = SessionManager()
    manager.vehicle_online("vehicle-001")
    session = manager.request_session("vehicle-001", "driver-console-001")
    print(json.dumps({"session_id": session.session_id, "state": session.state}, ensure_ascii=False, sort_keys=True))
    return 0


def _is_loopback_host(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


if __name__ == "__main__":
    raise SystemExit(main())
