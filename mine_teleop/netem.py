from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class WeakNetworkBaseline:
    delay_ms: tuple[int, ...]
    jitter_ms: tuple[int, ...]
    loss_percent: tuple[int, ...]
    bandwidth_mbps: tuple[int, ...]

    @classmethod
    def default(cls) -> "WeakNetworkBaseline":
        return cls(
            delay_ms=(50, 100, 200),
            jitter_ms=(20, 50),
            loss_percent=(1, 3, 5),
            bandwidth_mbps=(5, 10, 20),
        )

    def profiles(self) -> list["WeakNetworkProfile"]:
        profiles = []
        for delay_ms in self.delay_ms:
            for jitter_ms in self.jitter_ms:
                for loss_percent in self.loss_percent:
                    for bandwidth_mbps in self.bandwidth_mbps:
                        profiles.append(
                            WeakNetworkProfile(
                                name=(
                                    f"weak-{delay_ms}ms-jitter{jitter_ms}-"
                                    f"loss{_format_name_number(loss_percent)}-bandwidth{bandwidth_mbps}"
                                ),
                                delay_ms=delay_ms,
                                jitter_ms=jitter_ms,
                                loss_percent=loss_percent,
                                bandwidth_mbps=bandwidth_mbps,
                            )
                        )
        return profiles


@dataclass(frozen=True)
class WeakNetworkProfile:
    name: str
    delay_ms: int
    jitter_ms: int
    loss_percent: float
    bandwidth_mbps: int


@dataclass(frozen=True)
class TcNetemPlan:
    interface: str
    profile: WeakNetworkProfile

    @property
    def apply_command(self) -> str:
        return (
            f"sudo tc qdisc add dev {self.interface} root netem "
            f"delay {self.profile.delay_ms}ms {self.profile.jitter_ms}ms "
            f"loss {_format_percent(self.profile.loss_percent)} "
            f"rate {self.profile.bandwidth_mbps}mbit"
        )

    @property
    def clear_command(self) -> str:
        return f"sudo tc qdisc del dev {self.interface} root"

    @property
    def warning(self) -> str:
        return (
            "dry-run only: confirm the interface is a dedicated test link and "
            "that applying tc netem will not affect other network services"
        )


def _format_percent(value: float) -> str:
    if float(value).is_integer():
        return f"{int(value)}%"
    return f"{value}%"


def _format_name_number(value: float) -> str:
    if float(value).is_integer():
        return str(int(value))
    return f"{value:g}"
