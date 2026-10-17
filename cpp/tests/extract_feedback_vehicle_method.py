"""Extract actual vehicle publisher/startup methods for fake-Gst offline tests."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
p.add_argument('--startup-output', required=True)
p.add_argument('--result-output', required=True)
a = p.parse_args()
source = Path(a.source).read_text()

def extract(anchor, after=0):
    start = source.index(anchor, after)
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

# Compile the unchanged result initializer, RAII publisher and early guard.
# The fixture supplies only the surrounding runtime state and trace sink; this
# checks the actual published/MACed result, not source-string or trace presence.
start = source.index('      Json result_message = {{"event", "vehicle_control_result"}')
end = source.index('      const bool stale_safe_heartbeat =', start)
guard = extract('      if (stop_requested || control_inhibited ||', end)
Path(a.result_output).write_text(source[start:end] +
    '      static_cast<void>(verified_stop_only);\n'
    '      std::unique_lock lock(control_mutex);\n' + guard)
