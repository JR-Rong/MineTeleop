"""Exercise the executable's admission boundary without cameras or sockets."""
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
    (safe.replace('recording:\n  enabled: false', 'recording:\n  enabled: true'), False),
    (safe.replace('upload:\n  enabled: false', 'upload:\n  enabled: true'), False),
    (safe.replace('media_enabled: true', 'media_enabled: false'), False),
]
with tempfile.TemporaryDirectory(prefix='native-receiver-guard-') as directory:
    config = pathlib.Path(directory) / 'bench.yaml'
    for text, accepted in cases:
        config.write_text(text)
        result = subprocess.run([binary, '--config', str(config), '--validate-only'],
                                capture_output=True, text=True, timeout=3)
        assert (result.returncode == 0) == accepted, result.stdout + result.stderr
        if accepted:
            assert 'no runtime started' in result.stdout
    config.write_text(safe)
    evidence = pathlib.Path(directory) / 'existing.json'
    evidence.write_text('preserve-me')
    result = subprocess.run([binary, '--config', str(config), '--signaling-url', 'http://127.0.0.1:1', '--output', str(evidence)],
                            capture_output=True, text=True, timeout=3)
    assert result.returncode != 0 and 'new file' in result.stderr
    assert evidence.read_text() == 'preserve-me'
    for args, message in [
        (['--signaling-url', 'https://example.com'], 'loopback'),
        (['--signaling-url', 'http://127.0.0.1:1@example.com'], 'numeric'),
        (['--display', '--consumer-stall-ms', '200'], 'does not inject'),
        (['--wan-family', 'ipv6'], 'explicit local and remote'),
        (['--wan-local-address', '2606:4700::1'], 'explicit family'),
        (['--wan-family', 'ipv6', '--wan-local-address', 'fd00::1', '--wan-remote-address', '2606:4700::1'], 'public'),
        (['--wan-family', 'ipv6', '--wan-local-address', '2606:4700::1', '--wan-remote-address', '2606:4700:0:0:0:0:0:1'], 'must differ'),
    ]:
        result = subprocess.run([binary, '--config', str(config), '--validate-only', *args],
                                capture_output=True, text=True, timeout=3)
        assert result.returncode != 0 and message in result.stderr
    result = subprocess.run([binary, '--config', str(config), '--validate-only', '--wan-family', 'ipv6',
                             '--wan-local-address', '2606:4700::1', '--wan-remote-address', '2409:8d00::1'],
                            capture_output=True, text=True, timeout=3)
    assert result.returncode == 0, result.stdout + result.stderr
print('containment: 8 configurations, WAN endpoint guards and existing evidence checked')
