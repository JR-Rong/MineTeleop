#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.netem import TcNetemPlan, WeakNetworkBaseline, WeakNetworkProfile


def main() -> int:
    parser = argparse.ArgumentParser(description="Print dry-run tc netem commands for weak-network tests.")
    parser.add_argument("--interface", required=True)
    parser.add_argument("--matrix", action="store_true", help="Print the full documented weak-network matrix.")
    parser.add_argument("--delay-ms", type=int)
    parser.add_argument("--jitter-ms", type=int)
    parser.add_argument("--loss-percent", type=float)
    parser.add_argument("--bandwidth-mbps", type=int)
    args = parser.parse_args()

    if args.matrix:
        _print_plans(args.interface, WeakNetworkBaseline.default().profiles())
        return 0
    if (
        args.delay_ms is None
        or args.jitter_ms is None
        or args.loss_percent is None
        or args.bandwidth_mbps is None
    ):
        parser.error("--delay-ms, --jitter-ms, --loss-percent, and --bandwidth-mbps are required without --matrix")

    profile = WeakNetworkProfile(
        name=(
            f"weak-{args.delay_ms}ms-jitter{args.jitter_ms}-"
            f"loss{args.loss_percent:g}-bandwidth{args.bandwidth_mbps}"
        ),
        delay_ms=args.delay_ms,
        jitter_ms=args.jitter_ms,
        loss_percent=args.loss_percent,
        bandwidth_mbps=args.bandwidth_mbps,
    )
    _print_plans(args.interface, [profile])
    return 0


def _print_plans(interface: str, profiles) -> None:
    if not profiles:
        return
    first_plan = TcNetemPlan(interface=interface, profile=profiles[0])
    print(first_plan.warning)
    for profile in profiles:
        plan = TcNetemPlan(interface=interface, profile=profile)
        print(f"profile={profile.name}")
        print(f"apply={plan.apply_command}")
        print(f"clear={plan.clear_command}")


if __name__ == "__main__":
    raise SystemExit(main())
