"""Check the viewer's real config admission without opening a window or a session."""
import pathlib
import subprocess
import sys
import tempfile

binary, source = sys.argv[1:]
original = pathlib.Path(source).read_text()
safe = original.replace('control_enabled: true', 'control_enabled: false')
assert safe != original
cases = [
    (safe, True),
    (original, False),
    (safe.replace('type: mock', 'type: can'), False),
    (safe.replace('commissioning_mode: bench', 'commissioning_mode: field'), False),
    (safe.replace('max_speed_kph: 0', 'max_speed_kph: 1'), False),
    (safe.replace('media_enabled: true', 'media_enabled: false'), False),
    (safe.replace('recording:\n  enabled: false', 'recording:\n  enabled: true'), False),
    (safe.replace('upload:\n  enabled: false', 'upload:\n  enabled: true'), False),
]
with tempfile.TemporaryDirectory(prefix='native-viewer-guard-') as directory:
    path = pathlib.Path(directory) / 'bench.yaml'
    for content, accepted in cases:
        path.write_text(content)
        result = subprocess.run([binary, '--validate-config', str(path)], timeout=10)
        assert (result.returncode == 0) == accepted, (accepted, result.returncode)
    result = subprocess.run([binary, '--validate-config', str(path.with_name('missing.yaml'))], timeout=10)
    assert result.returncode != 0
print('Windows native viewer: nine configuration admission cases passed')
