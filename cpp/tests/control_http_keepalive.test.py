"""Exercise the real loopback control HTTP server on Windows and POSIX."""
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parents[2]
with socket.socket() as reserve:
    reserve.bind(('127.0.0.1', 0))
    port = reserve.getsockname()[1]
with tempfile.TemporaryFile() as log:
    process = subprocess.Popen([str(binary), '--config', str(root / 'configs/driver-console.dev.yaml'),
                                '--port', str(port), '--no-open-browser'], cwd=root, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 15
        while True:
            if process.poll() is not None:
                raise AssertionError('control service exited during startup')
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
            try:
                connection.request('GET', '/health')
                result = connection.getresponse()
                assert result.status == 200
                result.read()
                break
            except OSError:
                connection.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.05)
        source_port = connection.sock.getsockname()[1]
        for index in range(1, 512):
            # The server fails closed if input arrives during its final
            # post-response pipeline check, even if the client was very fast.
            time.sleep(.005)
            connection.request('GET', '/health')
            result = connection.getresponse()
            assert result.status == 200
            result.read()
            if index < 511:
                assert result.getheader('Connection').lower() == 'keep-alive'
                assert connection.sock.getsockname()[1] == source_port, 'HTTP connection was not reused'
            else:
                assert result.getheader('Connection').lower() == 'close', 'request bound not enforced'
        connection.close()
        with socket.create_connection(('127.0.0.1', port), timeout=3) as bad:
            bad.sendall(b'POST /health HTTP/1.1\r\nHost: 127.0.0.1\r\n'
                        b'Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n')
            status = bytearray()
            while not status.endswith(b'\r\n'):
                byte = bad.recv(1)
                assert byte, 'missing HTTP status'
                status.extend(byte)
                assert len(status) <= 256, 'unbounded HTTP status'
            assert status.startswith(b'HTTP/1.1 400 '), 'ambiguous framing accepted'
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if sys.exc_info()[0]:
            log.seek(0)
            print(log.read().decode(errors='replace'), file=sys.stderr)
print('PASS control HTTP connection reuse, request limit and framing')
