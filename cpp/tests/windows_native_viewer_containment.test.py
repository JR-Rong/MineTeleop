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
    mock = original.replace('id: vehicle-001', 'id: test_bench_unit').replace(
        '  - id: front_test\n    device: testsrc\n    realtime_profile: cpu_720p30',
        '  - id: front_test\n    device: testsrc\n    realtime_profile: cpu_720p30\n'
        '  - id: rear_test\n    device: testsrc\n    realtime_profile: cpu_720p30')
    assert mock != original and 'id: rear_test' in mock
    mock_cases = [
        (mock, True),
        (mock.replace('id: test_bench_unit', 'id: vehicle-001'), False),
        (mock.replace('control_enabled: true', 'control_enabled: false'), False),
        (mock.replace('type: mock', 'type: can'), False),
        (mock.replace('max_speed_kph: 0', 'max_speed_kph: 1'), False),
        (mock.replace('max_time_sync_uncertainty_ms: 25', 'max_time_sync_uncertainty_ms: 26'), False),
        (mock.replace('require_time_sync: true', 'require_time_sync: false'), False),
        (mock.replace('  - id: rear_test\n    device: testsrc\n    realtime_profile: cpu_720p30\n', ''), False),
    ]
    for content, accepted in mock_cases:
        path.write_text(content)
        result = subprocess.run([binary, '--validate-mock-config', str(path)], timeout=10)
        assert (result.returncode == 0) == accepted, (accepted, result.returncode)
print('Windows native viewer: nine configuration admission cases passed')
