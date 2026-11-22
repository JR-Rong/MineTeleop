"""Exercise the image's default driver-console arguments against the built CLI."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[2]
dockerfile = (root / 'deployments/container/Dockerfile.control').read_text()
args = json.loads(next(line[4:] for line in dockerfile.splitlines() if line.startswith('CMD ')))
assert args[0] == 'driver-console'
assert args[args.index('--host') + 1] == '127.0.0.1'
for host in ('0.0.0.0', '::', '192.0.2.1'):
    result = subprocess.run([str(binary), 'driver-console', '--host', host], cwd=root,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode != 0 and 'must bind to a loopback address' in result.stderr, result.stderr
args[args.index('--config') + 1] = str(root / 'configs/driver-console.dev.yaml')
with socket.socket() as reserve:
    reserve.bind(('127.0.0.1', 0))
    port = reserve.getsockname()[1]
args[args.index('--port') + 1] = str(port)
with tempfile.TemporaryFile() as log:
    process = subprocess.Popen([str(binary), *args], cwd=root, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            assert process.poll() is None, 'default console CMD exited'
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
            try:
                connection.request('GET', '/health')
                response = connection.getresponse()
                assert response.status == 200
                response.read()
                break
            except OSError:
                time.sleep(.05)
            finally:
                connection.close()
        else:
            raise AssertionError('default console CMD never became healthy')
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
        try:
            connection.request('POST', '/api/control-intent', '{}',
                               {'Content-Type': 'application/json', 'Origin': 'https://untrusted.invalid'})
            response = connection.getresponse()
            assert response.status == 403, 'entrypoint bypassed origin boundary'
            response.read()
        finally:
            connection.close()
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        log.seek(0)
        if sys.exc_info()[0]:
            print(log.read().decode(errors='replace'), file=sys.stderr)
print('Default Docker CMD starts on loopback; public binds and cross-origin POST rejected; process stopped')
