"""Extract the unchanged real WSS entry up to its first ordinary event branch.

This isolates authentication/error routing, not GStreamer or the full receiver.
"""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
source = Path(a.source).read_text()
start = source.index('  void handle_native_control_websocket_envelope(')
end = source.index('    if (event == "control_authority_lease") {', start)
helper = ''
helper_start = source.find('  void brake_direct_authority_locked(')
if helper_start >= 0:
    depth = 0
    for index in range(source.index('{', helper_start), len(source)):
        if source[index] == '{': depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                helper = source[helper_start:index + 1] + '\n'
                break
Path(a.output).write_text(helper + source[start:end] + '''
    static_cast<void>(websocket); static_cast<void>(native_feedback);
    static_cast<void>(envelope_received_at_utc_ms); static_cast<void>(envelope_received_monotonic_ms);
    static_cast<void>(event);
  }
''')
