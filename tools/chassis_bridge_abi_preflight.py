#!/usr/bin/env python3
"""Validate a staged chassis-bridge ABI without invoking a CAN-init entry point."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
from typing import Any


DEFAULT_CONTRACT = (
    Path(__file__).resolve().parents[1]
    / "abi/chassis-bridge/v6/preflight-contract.json"
)


class PreflightError(Exception):
    def __init__(self, code: str, message: str, **details: Any) -> None:
        super().__init__(message)
        self.code = code
        self.details = details


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a chassis bridge ABI from a library or staged package without "
            "calling an open/CAN-init function."
        )
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--bridge-library", type=Path)
    source.add_argument("--package-root", type=Path)
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument("--profile", default="vehicle_agent_pre_can")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise PreflightError("contract_missing", f"contract is missing: {path}") from error
    except json.JSONDecodeError as error:
        raise PreflightError(
            "contract_invalid_json",
            f"contract is not valid JSON: {path}: {error.msg}",
        ) from error
    if not isinstance(value, dict):
        raise PreflightError("contract_invalid", "contract root must be an object")
    return value


def require_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise PreflightError("contract_invalid", f"{field} must be a non-empty string")
    return value


def resolve_profile(
    profiles: dict[str, Any], name: str, seen: set[str] | None = None
) -> list[dict[str, Any]]:
    if seen is None:
        seen = set()
    if name in seen:
        raise PreflightError("contract_invalid", f"profile inheritance cycle at {name}")
    seen.add(name)
    profile = profiles.get(name)
    if not isinstance(profile, dict):
        raise PreflightError("unknown_profile", f"contract profile does not exist: {name}")
    checks: list[dict[str, Any]] = []
    parent = profile.get("extends")
    if parent is not None:
        checks.extend(resolve_profile(profiles, require_string(parent, "profile.extends"), seen))
    own_checks = profile.get("checks")
    if not isinstance(own_checks, list):
        raise PreflightError("contract_invalid", f"profile {name} checks must be an array")
    for check in own_checks:
        if not isinstance(check, dict):
            raise PreflightError("contract_invalid", f"profile {name} has a non-object check")
        symbol = require_string(check.get("symbol"), "check.symbol")
        kind = require_string(check.get("kind"), "check.kind")
        if kind not in {"symbol", "u32_query"}:
            raise PreflightError("contract_invalid", f"unsupported check kind for {symbol}: {kind}")
        if kind == "u32_query" and (
            not isinstance(check.get("expected"), int) or check["expected"] < 0
        ):
            raise PreflightError(
                "contract_invalid", f"u32 query {symbol} must have a non-negative integer expected value"
            )
        checks.append(check)
    return checks


def resolve_bridge_path(arguments: argparse.Namespace, contract: dict[str, Any]) -> Path:
    if arguments.bridge_library is not None:
        return arguments.bridge_library
    package = contract.get("supported_package")
    if not isinstance(package, dict):
        raise PreflightError("contract_invalid", "supported_package must be an object")
    relative_path = require_string(package.get("bridge_relative_path"), "bridge_relative_path")
    relative = Path(relative_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise PreflightError("contract_invalid", "bridge_relative_path must be a safe relative path")
    return arguments.package_root / relative


def open_library(path: Path) -> ctypes.CDLL:
    if not path.is_file():
        raise PreflightError("bridge_missing", f"bridge library is missing or not a file: {path}")
    try:
        if os.name == "nt":
            return ctypes.WinDLL(str(path))
        return ctypes.CDLL(str(path))
    except OSError as error:
        raise PreflightError("bridge_load_failed", f"failed to load bridge library: {path}: {error}") from error


def verify(library: ctypes.CDLL, checks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    seen_symbols: set[str] = set()
    for check in checks:
        symbol_name = check["symbol"]
        if symbol_name in seen_symbols:
            raise PreflightError("contract_invalid", f"duplicate required symbol: {symbol_name}")
        seen_symbols.add(symbol_name)
        try:
            symbol = getattr(library, symbol_name)
        except AttributeError as error:
            raise PreflightError(
                "missing_required_symbol",
                f"bridge is missing required symbol: {symbol_name}",
                symbol=symbol_name,
            ) from error
        if check["kind"] == "symbol":
            results.append({"symbol": symbol_name, "kind": "symbol", "passed": True})
            continue
        symbol.argtypes = []
        symbol.restype = ctypes.c_uint32
        actual = int(symbol())
        expected = check["expected"]
        if actual != expected:
            raise PreflightError(
                "abi_value_mismatch",
                f"bridge query {symbol_name} returned {actual}, expected {expected}",
                symbol=symbol_name,
                actual=actual,
                expected=expected,
            )
        results.append(
            {
                "symbol": symbol_name,
                "kind": "u32_query",
                "actual": actual,
                "expected": expected,
                "passed": True,
            }
        )
    return results


def emit(value: dict[str, Any]) -> None:
    print(json.dumps(value, sort_keys=True, separators=(",", ":")))


def main() -> int:
    arguments = parse_args()
    try:
        contract = load_json(arguments.contract)
        contract_id = require_string(contract.get("contract_id"), "contract_id")
        profiles = contract.get("profiles")
        if not isinstance(profiles, dict):
            raise PreflightError("contract_invalid", "profiles must be an object")
        checks = resolve_profile(profiles, arguments.profile)
        bridge_path = resolve_bridge_path(arguments, contract).resolve()
        library = open_library(bridge_path)
        results = verify(library, checks)
        emit(
            {
                "bridge_library": str(bridge_path),
                "can_init_called": False,
                "contract_id": contract_id,
                "passed": True,
                "profile": arguments.profile,
                "results": results,
            }
        )
        return 0
    except PreflightError as error:
        emit(
            {
                "can_init_called": False,
                "error": {"code": error.code, "details": error.details, "message": str(error)},
                "passed": False,
                "profile": arguments.profile,
            }
        )
        return 2


if __name__ == "__main__":
    sys.exit(main())
