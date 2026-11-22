"""Local subprocess/pipe regressions; no native runtime, credentials or network."""

import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import redirect_stdout
from types import SimpleNamespace
import unittest
from unittest.mock import patch


source = Path(__file__).resolve().parents[1] / 'test' / 'process_capture.py'
spec = importlib.util.spec_from_file_location('process_capture', source)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


@unittest.skipUnless(os.name == 'posix', 'local process group tests require POSIX')
class ProcessCaptureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def child(self, code, **kwargs):
        return capture.LoggedProcess(
            [sys.executable, '-u', '-c', code],
            stdout_path=self.root / 'stdout.log', stderr_path=self.root / 'stderr.log',
            env={'PATH': os.environ.get('PATH', ''), 'PYTHONDONTWRITEBYTECODE': '1'},
            cleanup_seconds=0.2, **kwargs)

    def assert_closed(self, child):
        self.assertIsNotNone(child.process.returncode)
        self.assertTrue(child.process.stdout.closed and child.process.stderr.closed)
        self.assertTrue(all(not item.thread.is_alive() for item in child.captures))

    def test_secret_formats_and_diagnostic_prose(self):
        forms = [
            'device_token=CANARY', 'password:CANARY', 'secret=CANARY',
            'credential=CANARY', 'Authorization: Bearer CANARY',
            'Authorization: Basic CANARY', 'https://user:CANARY@example.invalid/',
            '{"device_token":"CANARY"}', "{'password':'CANARY'}",
            'MINE_TELEOP_DRIVER_PASSWORD=CANARY', 'ice_pwd=CANARY', 'ice_ufrag=CANARY',
            'a=ice-pwd:CANARY', 'a=ice-ufrag:CANARY',
            'token="escaped\\\"CANARY"', 'password="CANARY', "token='CANARY",
            '{"nested":{"password":"CANARY"}}',
        ]
        for value in forms:
            with self.subTest(value=value):
                self.assertNotIn('CANARY', capture.redact_log(value))
        self.assertEqual(capture.redact_log('cannot read device token file'), 'cannot read device token file')
        self.assertNotIn('PUBLIC_EXACT', capture.redact_log('unlabelled PUBLIC_EXACT', ['PUBLIC_EXACT']))

    def test_over_64_mib_does_not_stop_workload_and_has_bounded_files_and_tail(self):
        # Same long-word shape that previously made redaction quadratic. The
        # deadline guards throughput; do not relax it to hide a regex regression.
        code = ("import os\nline=b'x'*4096+b'token'*820+b'\\n'\n"
                "os.write(2,b'PUBLIC_ERROR_RETAINED\\n')\n"
                "for i in range(8300): os.write(1,line)\n"
                "os.write(1,b'PUBLIC_FINISHED\\n')\n")
        with self.child(code, max_bytes=1024 * 1024, files=3, tail_bytes=16384) as child:
            self.assertEqual(child.wait(40), 0)
        self.assert_closed(child)
        stdout, stderr = child.captures
        self.assertGreater(stdout.raw_bytes, 64 * 1024 * 1024)
        self.assertGreater(stdout.rotations, 1)
        self.assertEqual(stdout.dropped_lines, 0)
        self.assertIn('PUBLIC_FINISHED', stdout.text())
        self.assertIn('PUBLIC_ERROR_RETAINED', stderr.text())
        self.assertLessEqual(len(stdout.text().encode()), 16384)
        for path in stdout.paths + stderr.paths:
            self.assertLessEqual(path.stat().st_size, 1024 * 1024)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_split_secret_and_unterminated_failure_tail_preserve_rc(self):
        code = ("import os,sys\nos.write(1,b'device_to');os.write(1,b'ken=CANARY\\n')\n"
                "os.write(2,b'time_sync_failed uncertainty=58 limit=25\\npassword=CANARY')\n"
                "sys.exit(7)\n")
        with self.child(code) as child:
            self.assertEqual(child.wait(5), 7)
        self.assert_closed(child)
        saved = ''.join(path.read_text() for item in child.captures for path in item.paths)
        self.assertNotIn('CANARY', saved)
        self.assertIn('uncertainty=58 limit=25', saved)
        self.assertIn('[REDACTED]', saved)

    def test_oversized_line_discards_through_newline_even_at_chunk_boundary(self):
        # Newline is the last byte in a read block. It must not look like EOF
        # after discarding, or the next valid record would silently disappear.
        payload = b'x' * (65536 * 18 - 16) + b'CANARY_SUFFIX!!\n'
        self.assertEqual(len(payload) % 65536, 0)
        stream = io.BytesIO(payload + b'PUBLIC_AFTER_DROP\n' + b'password=CANARY')
        failed = threading.Event()
        item = capture.LogCapture(stream, self.root / 'capture', failed, max_bytes=1024)
        item.thread.join(3)
        self.assertFalse(item.thread.is_alive())
        self.assertFalse(failed.is_set())
        self.assertEqual(item.dropped_lines, 1)
        self.assertIn('PUBLIC_AFTER_DROP', item.text())
        self.assertNotIn('CANARY', item.text())

    def test_expanded_utf8_line_cannot_exceed_disk_bound(self):
        failed = threading.Event()
        item = capture.LogCapture(io.BytesIO(b'\xff' * 200 + b'\npublic\n'),
                                  self.root / 'capture', failed, max_bytes=256)
        item.thread.join(3)
        self.assertFalse(failed.is_set())
        self.assertEqual(item.dropped_lines, 1)
        self.assertTrue(all(path.stat().st_size <= 256 for path in item.paths))
        self.assertIn('public', item.text())

    def test_existing_rotated_slot_is_not_overwritten_and_failed_start_reaps_child(self):
        target = self.root / 'stderr.log.1'
        target.write_text('PUBLIC_EXISTING')
        children = []
        original = subprocess.Popen

        def spawn(*args, **kwargs):
            child = original(*args, **kwargs)
            children.append(child)
            return child

        with patch.object(capture.subprocess, 'Popen', side_effect=spawn):
            with self.assertRaises(FileExistsError):
                self.child('import time;time.sleep(30)')
        self.assertEqual(target.read_text(), 'PUBLIC_EXISTING')
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].returncode)

    def test_sink_error_stops_and_reaps_only_owned_child(self):
        with patch.object(capture.LogCapture, '_write', side_effect=OSError('synthetic sink failure')):
            with self.assertRaisesRegex(RuntimeError, 'test_log_capture_failed'):
                with self.child("import time;print('PUBLIC_READY');time.sleep(30)") as child:
                    child.wait(5)
        self.assert_closed(child)
        self.assertTrue(child.failed.is_set())

    def test_timeout_kills_term_ignoring_child_and_joins_readers(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            with self.child("import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                            "print('PUBLIC_READY');time.sleep(30)") as child:
                deadline = time.monotonic() + 5
                while 'PUBLIC_READY' not in child.captures[0].text():
                    self.assertLess(time.monotonic(), deadline, 'fixture did not install TERM handler')
                    time.sleep(0.01)
                child.wait(0.05)
        self.assert_closed(child)
        self.assertEqual(child.process.returncode, -9)
        child.close()  # Cleanup is idempotent and cannot extend a finished run.

    def test_caller_exception_still_reaps_child(self):
        with self.assertRaisesRegex(OSError, 'synthetic metadata failure'):
            with self.child('import time;time.sleep(30)') as child:
                raise OSError('synthetic metadata failure')
        self.assert_closed(child)

    def test_descendant_held_pipes_do_not_block_cleanup_after_leader_exits(self):
        code = ("import subprocess,sys\nsubprocess.Popen([sys.executable,'-c',"
                "'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(30)'])\n"
                "print('PUBLIC_LEADER_FINISHED')\n")
        with self.child(code) as child:
            self.assertEqual(child.wait(5), 0)
        self.assert_closed(child)

    def test_stdin_defaults_to_eof_and_never_inherits_launcher_input(self):
        with self.child("import sys;print('EOF' if not sys.stdin.buffer.read(1) else 'UNEXPECTED')") as child:
            self.assertEqual(child.wait(5), 0)
        self.assertIn('EOF', child.captures[0].text())
        self.assert_closed(child)
        for implicit in (None, subprocess.PIPE):
            with self.assertRaisesRegex(ValueError, 'explicit input'):
                self.child('pass', stdin=implicit)

    def test_explicit_terminal_stdin_receives_public_marker(self):
        # A script read from a here-document has EOF on its own stdin. Supply
        # the caller-owned terminal explicitly instead of inheriting that EOF.
        import pty
        master, slave = pty.openpty()
        try:
            with self.child("import os,sys;line=sys.stdin.readline();"
                            "print('PUBLIC_RECEIVED' if os.isatty(0) and line=='PUBLIC_MARKER\\n' else 'FAIL')",
                            stdin=slave) as child:
                os.write(master, b'PUBLIC_MARKER\n')
                self.assertEqual(child.wait(5), 0)
            self.assertIn('PUBLIC_RECEIVED', child.captures[0].text())
            self.assert_closed(child)
        finally:
            os.close(master)
            os.close(slave)

    def test_smoke_consumer_preserves_exit_and_retry_logs_without_real_network(self):
        # Run the real smoke orchestration with actual owned Python children.
        # Only HTTP/port allocation and the native executable are fixtures.
        smoke_spec = importlib.util.spec_from_file_location('smoke_fixture', source.with_name('control_smoke_loopback.py'))
        smoke = importlib.util.module_from_spec(smoke_spec)
        with patch.dict(sys.modules, {'process_capture': capture}):
            smoke_spec.loader.exec_module(smoke)
        binary = self.root / 'mine-teleop'
        binary.write_text('#!' + sys.executable + '\nimport sys,time\n'
                          "assert not sys.stdin.buffer.read(1)\n"
                          "print('password=loopback-synthetic-password',flush=True)\n"
                          "if sys.argv[1]=='control-smoke': sys.exit(7)\n"
                          'time.sleep(30)\n')
        binary.chmod(0o700)
        children = []

        def launch(*args, **kwargs):
            child = capture.LoggedProcess(*args, **kwargs)
            children.append(child)
            return child

        class Health:
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                pass

        reservation = SimpleNamespace(bind=lambda _addr: None, getsockname=lambda: ('127.0.0.1', 12345),
                                      close=lambda: None)
        argv = ['fixture', '--binary', str(binary), '--output-dir', str(self.root / 'results')]
        with patch.object(sys, 'argv', argv), patch.object(smoke.platform, 'system', return_value='Linux'), \
                patch.object(smoke.socket, 'if_nameindex', return_value=[(1, 'lo')]), \
                patch.object(smoke.socket, 'socket', return_value=reservation), \
                patch.object(smoke.urllib.request, 'build_opener', return_value=SimpleNamespace(open=lambda *_a, **_k: Health())), \
                patch.object(smoke, 'LoggedProcess', side_effect=launch), redirect_stdout(io.StringIO()):
            self.assertEqual(smoke.main(), 7)
            self.assertEqual(smoke.main(), 7)
        runs = list((self.root / 'results').iterdir())
        self.assertEqual(len(runs), 2)
        for run in runs:
            result = json.loads((run / 'result.json').read_text())
            self.assertEqual(result['exit_code'], 7)
            self.assertEqual(len(result['logs']), 6)
            self.assertFalse(result['video_observation_performed'])
            self.assertNotIn('loopback-synthetic-password', (run / 'control-smoke.stdout.jsonl').read_text())
        for child in children:
            self.assert_closed(child)


if __name__ == '__main__':
    unittest.main()
