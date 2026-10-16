"""Extract actual vehicle publisher/startup methods for fake-Gst offline tests."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
p.add_argument('--startup-output', required=True)
a = p.parse_args()
source = Path(a.source).read_text()

def extract(anchor):
    start = source.index(anchor)
    depth = 0
    for index in range(source.index('{', start), len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1] + '\n'
    raise RuntimeError('vehicle method is incomplete: ' + anchor)

helper = extract('  bool cloud_control_authority_live_locked() const {')
Path(a.output).write_text(helper + extract('  void send_control_status_locked(Json message) {'))
Path(a.startup_output).write_text(helper + extract('  [[nodiscard]] bool start_control_service() {'))
