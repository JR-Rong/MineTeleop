"""Compile unchanged vehicle authority methods with bounded local test doubles."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
p.add_argument('--lease-cache-output')
a = p.parse_args()
source = Path(a.source).read_text()

def method(name):
    start = source.find(('  bool ' if name == 'cloud_control_authority_live_locked' else '  void ') + name + '(')
    if start < 0:
        return ''
    depth = 0
    for index in range(source.index('{', start), len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if not depth:
                return source[start:index + 1]
    raise RuntimeError('incomplete authority method: ' + name)

Path(a.output).write_text('\n'.join(method(name) for name in [
    'cloud_control_authority_live_locked', 'brake_direct_authority_locked', 'mark_direct_authority_worker_failed',
    'direct_authority_loop', 'tick_control_service']) + '\n')
if a.lease_cache_output:
    Path(a.lease_cache_output).write_text(method('observe_lease') + '\n')
