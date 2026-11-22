"""Bounded, redacted logs for owned local test subprocesses (POSIX only).

This is test tooling, not a field launcher or an authorization lifetime manager.
Callers supply argv/environment and an explicit stdin source when one is needed.
Each stream has independent cyclic log slots, so stdout cannot evict stderr.
"""

from collections import deque
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import threading
import time


# Boundaries avoid retrying the greedy key/scheme match at every character of
# long ordinary words. Keep full lines until redaction, including an EOF tail.
_KEY = re.compile(r'(?<![A-Za-z0-9_-])(["\']?)([A-Za-z0-9_-]+)\1\s*[:=]\s*')
_SENSITIVE = ('token', 'password', 'secret', 'credential', 'authorization', 'ice_pwd', 'ice_ufrag')
_VALUE = re.compile(r'"(?:[^"\\]|\\.)*(?:"|$)|\'(?:[^\'\\]|\\.)*(?:\'|$)|[^\s,;}\]]+')
_AUTH = re.compile(r'(authorization\s*:\s*(?:bearer|basic)\s+)[^\s,;"\']+', re.I)
_URI = re.compile(r'(?<![a-z0-9+.-])([a-z][a-z0-9+.-]*://)[^\s/@]+:[^\s/@]+@', re.I)


def redact_log(text, secrets=()):
    for value in secrets:
        if value:
            text = text.replace(value, '[REDACTED]')
    text = _AUTH.sub(r'\1[REDACTED]', text)
    # Scan keys once, then inspect their names. A single regex with greedy
    # prefix/keyword/suffix repeats work for long words containing many tokens.
    pieces, end = [], 0
    for match in _KEY.finditer(text):
        if match.start() < end or not any(name in match[2].lower().replace('-', '_') for name in _SENSITIVE):
            continue
        value = _VALUE.match(text, match.end())
        if value:
            pieces.extend((text[end:match.end()], '"[REDACTED]"'))
            end = value.end()
    text = ''.join(pieces) + text[end:]
    return _URI.sub(r'\1[REDACTED]@', text)


class LogCapture:
    """Drain a pipe without killing its producer merely for producing logs.

    Slots are path, path.1, ...; rotations % len(paths) identifies the newest.
    All slots are created exclusively at startup and kept open, so rotation
    never follows a replaced path or overwrites someone else's existing log.
    """

    def __init__(self, stream, path, failed, *, secrets=(), max_bytes=8 * 1024 * 1024,
                 files=4, line_bytes=1024 * 1024, tail_bytes=1024 * 1024):
        if min(max_bytes, line_bytes, tail_bytes) < 256 or not 1 <= files <= 16:
            raise ValueError('invalid test log limits')
        self.stream = stream
        self.failed = failed
        self.secrets = secrets
        self.max_bytes, self.line_bytes, self.tail_bytes = max_bytes, line_bytes, tail_bytes
        self.paths = [Path(str(path) + (f'.{i}' if i else '')) for i in range(files)]
        self.outputs = []
        self.error = None
        self.cancelled = threading.Event()
        self.raw_bytes = self.rotations = self.dropped_lines = self.written_bytes = 0
        self._slot_bytes = self._tail_size = 0
        self._tail = deque()
        self._lock = threading.Lock()
        try:
            for target in self.paths:
                fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                self.outputs.append(os.fdopen(fd, 'wb'))
        except BaseException:
            for output in self.outputs:
                output.close()
            raise
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def text(self):
        with self._lock:
            return b''.join(self._tail).decode('utf-8', 'replace')

    def _write(self, encoded):
        output = self.outputs[self.rotations % len(self.outputs)]
        if self._slot_bytes + len(encoded) > self.max_bytes:
            self.rotations += 1
            output = self.outputs[self.rotations % len(self.outputs)]
            output.seek(0)
            output.truncate()
            self._slot_bytes = 0
        output.write(encoded)
        output.flush()
        self._slot_bytes += len(encoded)
        self.written_bytes += len(encoded)
        with self._lock:
            self._tail.append(encoded)
            self._tail_size += len(encoded)
            while self._tail_size > self.tail_bytes:
                self._tail_size -= len(self._tail.popleft())

    def _drop(self):
        self.dropped_lines += 1
        self._write(b'{"event":"test_log_line_dropped","reason":"oversized_line"}\n')

    def _line(self, raw):
        if len(raw) > self.line_bytes:
            self._drop()
            return
        clean = redact_log(raw.decode('utf-8', 'replace'), self.secrets).encode()
        if len(clean) > self.max_bytes:
            self._drop()  # UTF-8 replacement/redaction can expand a short raw line.
        else:
            self._write(clean)

    def _fail(self):
        self.error = 'test_log_capture_failed'
        self.failed.set()

    def _run(self):
        pending = bytearray()
        discard = False
        try:
            try:
                descriptor = self.stream.fileno()
            except (OSError, AttributeError):
                descriptor = None
            while True:
                if self.cancelled.is_set():
                    self._fail()
                    break
                if descriptor is not None and not select.select([descriptor], [], [], 0.1)[0]:
                    continue
                block = self.stream.read1(65536)
                eof = not block
                self.raw_bytes += len(block)
                if discard:
                    if b'\n' not in block:
                        if not block:
                            break
                        continue
                    block = block.split(b'\n', 1)[1]
                    discard = False
                pending.extend(block)
                while b'\n' in pending:
                    line, _, pending = pending.partition(b'\n')
                    self._line(line + b'\n')
                if len(pending) > self.line_bytes:
                    pending.clear()
                    discard = True
                    self._drop()
                if eof:
                    if pending and not discard:
                        self._line(pending)
                    break
        except Exception:
            self._fail()
        finally:
            for output in self.outputs:
                try:
                    output.close()
                except OSError:
                    self._fail()


class LoggedProcess:
    """Own one local process group, its input descriptor and both log readers.

    Use as a context manager (or register close in ExitStack immediately).
    A wait timeout, caller exception or log failure still reaches bounded
    TERM/KILL cleanup and wait. No shell, network or credential policy is added.
    """

    def __init__(self, argv, *, stdout_path, stderr_path, cwd=None, env=None,
                 stdin=subprocess.DEVNULL, cleanup_seconds=2, **capture_options):
        if os.name != 'posix':
            raise ValueError('local test process groups require POSIX')
        if stdin is None or stdin == subprocess.PIPE:
            raise ValueError('supply an explicit input descriptor or DEVNULL')
        if not 0 < cleanup_seconds <= 5:
            raise ValueError('invalid test cleanup grace')
        self.cleanup_seconds = cleanup_seconds
        self.failed = threading.Event()
        self.captures = []
        self.closed = False
        self.process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=stdin,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        close_fds=True, start_new_session=True)
        try:
            for stream, path in ((self.process.stdout, stdout_path), (self.process.stderr, stderr_path)):
                self.captures.append(LogCapture(stream, path, self.failed, **capture_options))
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, kind, *_args):
        self.close()
        if kind is None:
            self.check_logs()

    def check_logs(self):
        if self.failed.is_set():
            raise RuntimeError('test_log_capture_failed')

    def poll(self):
        self.check_logs()
        return self.process.poll()

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            if time.monotonic() >= deadline:
                # Do not copy argv (which can contain synthetic secrets) into errors.
                raise subprocess.TimeoutExpired('local test child', timeout)
            time.sleep(0.01)
        return self.process.returncode

    def _signal(self, sig):
        try:
            os.killpg(self.process.pid, sig)
        except ProcessLookupError:
            pass

    def close(self):
        if self.closed:
            return
        self.closed = True
        self._signal(signal.SIGCONT)
        self._signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=self.cleanup_seconds)
        except subprocess.TimeoutExpired:
            self._signal(signal.SIGKILL)
            self.process.wait(timeout=self.cleanup_seconds)
        # Descendants may retain the pipes after the leader exits. Terminate
        # the owned group even in that case; never wait indefinitely for EOF.
        for capture in self.captures:
            capture.thread.join(timeout=0.1)
        self._signal(signal.SIGKILL)
        for capture in self.captures:
            capture.thread.join(timeout=self.cleanup_seconds)
            if capture.thread.is_alive():
                self.failed.set()
                capture.cancelled.set()
                capture.thread.join(timeout=0.5)
        for stream in (self.process.stdout, self.process.stderr):
            stream.close()
