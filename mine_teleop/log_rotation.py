from __future__ import annotations

from pathlib import Path


def rotate_file_if_needed(path: Path, incoming_bytes: int, max_bytes: int | None, backup_count: int) -> None:
    if max_bytes is None or backup_count <= 0 or not path.exists():
        return
    if path.stat().st_size + incoming_bytes <= max_bytes:
        return
    oldest = backup_path(path, backup_count)
    if oldest.exists():
        oldest.unlink()
    for index in range(backup_count - 1, 0, -1):
        source = backup_path(path, index)
        if source.exists():
            source.replace(backup_path(path, index + 1))
    path.replace(backup_path(path, 1))


def backup_path(path: Path, index: int) -> Path:
    return path.with_name(f"{path.name}.{index}")
