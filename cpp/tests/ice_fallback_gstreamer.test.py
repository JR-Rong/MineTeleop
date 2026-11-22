#!/usr/bin/env python3
"""Run only in a network-isolated environment. Local coturn is a fault fixture,
not proof of WAN connectivity, native rendering, or full vehicle safety recovery.
"""
import base64
import hashlib
import hmac
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time

turn = shutil.which('turnserver')
if not turn:
    print('SKIP: coturn required for isolated TURN fallback integration')
    sys.exit(77)
username = str(int(time.time())+3600)+':fixture'
credential = base64.b64encode(hmac.new(b'fixture-shared-key', username.encode(), hashlib.sha1).digest()).decode()
with tempfile.TemporaryDirectory(prefix='mine-ice-fallback-') as directory:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    log = pathlib.Path(directory)/'turn.log'
    with log.open('w+') as output:
        server = subprocess.Popen([turn, '-n', '--no-cli', '--no-tls', '--no-dtls', '--no-tcp',
            '--listening-ip=127.0.0.1', '--relay-ip=127.0.0.1', f'--listening-port={port}',
            '--use-auth-secret', '--realm=isolated.test', '--static-auth-secret=fixture-shared-key',
            '--allow-loopback-peers', '--min-port=49152', '--max-port=49252',
            '--pidfile='+directory+'/turn.pid', '--log-file=stdout'], stdout=output, stderr=subprocess.STDOUT)
        try:
            time.sleep(0.5)
            if server.poll() is not None:
                raise RuntimeError('isolated coturn failed to start')
            result = subprocess.run([sys.argv[1], str(port), username, credential], timeout=60)
            if result.returncode:
                output.flush()
                print(log.read_text()[-6000:])
            sys.exit(result.returncode)
        finally:
            server.terminate()
            try: server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
