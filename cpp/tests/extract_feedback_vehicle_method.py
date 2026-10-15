"""Extract one unchanged vehicle method for a fake-Gst compile/run fixture."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
source = Path(a.source).read_text()
start = source.index('  void send_control_status_locked(Json message) {')
depth = 0
for index in range(source.index('{', start), len(source)):
    if source[index] == '{':
        depth += 1
    elif source[index] == '}':
        depth -= 1
        if depth == 0:
            Path(a.output).write_text(source[start:index + 1] + '\n')
            break
else:
    raise RuntimeError('vehicle feedback method is incomplete')
