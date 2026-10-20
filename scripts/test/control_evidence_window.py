"""Product-clock boundaries for auditing retired control-command evidence.

A closed archive contains real fault and cleanup records until process exit.
Keep those records within the planned product-clock window. An observer host's
wall-clock finish is not a boundary for product-clock command timestamps.
This helper selects evidence; it never awards a test pass or application result.
"""

import math


def _finite_number(value):
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and math.isfinite(value)


def product_window_cutoff_ms(started_at_utc_ms, duration_seconds):
    """Use the verified product start anchor and the planned duration only."""
    if not _finite_number(started_at_utc_ms) or started_at_utc_ms < 0:
        raise ValueError('invalid product-clock start')
    if not _finite_number(duration_seconds) or duration_seconds <= 0:
        raise ValueError('invalid observation duration')
    cutoff = started_at_utc_ms + duration_seconds * 1000
    if not _finite_number(cutoff):
        raise ValueError('invalid product-clock cutoff')
    return cutoff


def dispatches_before_product_cutoff(commands, cutoff_ms, issues):
    """Retain actual dispatches, exposing unknown times instead of inventing them.

    Non-dispatch stages retain their own evidence and are joined by sequence by
    the caller. Including cleanup records does not extend healthy control time.
    """
    if not _finite_number(cutoff_ms) or cutoff_ms < 0:
        raise ValueError('invalid product-clock cutoff')
    selected = []
    for command in commands:
        if command.get('stage') in ('send_completed', 'send_failed', 'send_skipped'):
            stamp = command.get('command_sent_at_utc_ms')
            if not _finite_number(stamp) or stamp < 0:
                issues.append({'kind': 'dispatch_timestamp_unavailable', 'seq': command.get('seq')})
                continue
            if stamp > cutoff_ms:
                continue
        selected.append(command)
    return selected
