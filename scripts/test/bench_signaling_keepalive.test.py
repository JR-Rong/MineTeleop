#!/usr/bin/env python3
"""Exercise the isolated bench proxy's HTTP reuse and WebSocket forwarding.

Runs only on loopback, using an ephemeral mock upstream and temporary Caddy
storage. It does not prove the production control protocol or vehicle safety.
"""
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'configs/bench-signaling-keepalive.Caddyfile'


def reserve_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def read_headers(sock):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError('peer closed during HTTP headers')
        data += chunk
        if len(data) > 16384:
            raise RuntimeError('HTTP header limit exceeded')
    return data


def read_exact(sock, size):
    data = b''
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError('peer closed during WebSocket frame')
        data += chunk
    return data


def main():
    upstream = socket.socket()
    upstream.bind(('127.0.0.1', 0))
    upstream.listen(4)
    upstream.settimeout(8)
    proxy_port = reserve_port()
    failures = []
    requests = []

    def serve():
        try:
            for _ in range(3):
                conn, _ = upstream.accept()
                with conn:
                    conn.settimeout(3)
                    headers = read_headers(conn)
                    requests.append(headers.split(b'\r\n', 1)[0])
                    if b'Upgrade: websocket' in headers or b'Upgrade: Websocket' in headers:
                        key = next(line.split(b':', 1)[1].strip() for line in headers.split(b'\r\n')
                                   if line.lower().startswith(b'sec-websocket-key:'))
                        accept = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                        conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                                     b'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
                        frame = read_exact(conn, 8)
                        if frame != b'\x81\x82\x01\x02\x03\x04ik':
                            raise RuntimeError('WebSocket frame was not forwarded unchanged')
                        conn.sendall(b'\x81\x02ok')
                    else:
                        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok')
        except Exception as error:
            failures.append(str(error))
        finally:
            upstream.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix='mine-teleop-caddy-test-') as temp:
        config = Path(temp) / 'Caddyfile'
        text = CONFIG.read_text().replace('127.0.0.1:28766', f'127.0.0.1:{proxy_port}').replace(
            '127.0.0.1:28765', f'127.0.0.1:{upstream.getsockname()[1]}')
        config.write_text(text)
        adapted = subprocess.run(['caddy', 'adapt', '--adapter', 'caddyfile', '--config', str(config)],
                                 check=True, capture_output=True, text=True)
        servers = json.loads(adapted.stdout)['apps']['http']['servers']
        assert servers and all(server['listen'] == [f'127.0.0.1:{proxy_port}']
                               for server in servers.values()), 'proxy must listen only on loopback'
        env = dict(os.environ, XDG_CONFIG_HOME=temp, XDG_DATA_HOME=temp)
        proc = subprocess.Popen(['caddy', 'run', '--adapter', 'caddyfile', '--config', str(config)],
                                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while True:
                if proc.poll() is not None:
                    raise RuntimeError('Caddy exited during startup')
                try:
                    with socket.create_connection(('127.0.0.1', proxy_port), timeout=.1):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Caddy did not start')
                    time.sleep(.02)
            client = http.client.HTTPConnection('127.0.0.1', proxy_port, timeout=3)
            client.request('GET', '/time?client_send_ms=1')
            response = client.getresponse()
            assert response.status == 200 and response.read() == b'ok'
            first_socket = client.sock
            assert first_socket is not None
            client.request('GET', '/time?client_send_ms=2')
            response = client.getresponse()
            assert response.status == 200 and response.read() == b'ok' and client.sock is first_socket
            client.close()
            with socket.create_connection(('127.0.0.1', proxy_port), timeout=3) as ws:
                ws.sendall(b'GET /signaling/test/ws HTTP/1.1\r\nHost: 127.0.0.1\r\n'
                           b'Connection: Upgrade\r\nUpgrade: websocket\r\n'
                           b'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n')
                assert read_headers(ws).startswith(b'HTTP/1.1 101 ')
                ws.sendall(b'\x81\x82\x01\x02\x03\x04ik')
                assert read_exact(ws, 4) == b'\x81\x02ok'
            thread.join(timeout=4)
            assert not thread.is_alive() and not failures, failures
            assert len(requests) == 3 and requests[2].startswith(b'GET /signaling/test/ws ')
            print('PASS: loopback-only listener, client HTTP reuse and WebSocket forwarding')
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            upstream.close()


if __name__ == '__main__':
    main()
