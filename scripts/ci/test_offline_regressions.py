import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


source = Path(__file__).resolve().parents[1] / 'test' / 'run_offline_regressions.py'
spec = importlib.util.spec_from_file_location('offline_regressions', source)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class OfflineRunnerSafetyTests(unittest.TestCase):
    def test_ambient_field_and_external_test_overrides_do_not_reach_children(self):
        with patch.dict(os.environ, {'MINE_TELEOP_TEST_WSS_URL': 'wss://field.invalid',
                                    'MINE_TELEOP_TEST_NATIVE_ROOT': '/field/package',
                                    'MINE_TELEOP_DRIVER_PASSWORD': 'fixture-secret',
                                    'NODE_OPTIONS': '--require=/injected',
                                    'LD_PRELOAD': '/injected',
                                    'DYLD_INSERT_LIBRARIES': '/injected',
                                    'PATH': '/tools'}, clear=True):
            env = runner.clean_environment()
        self.assertEqual(env['PATH'], '/tools')
        self.assertEqual(env['PYTHONDONTWRITEBYTECODE'], '1')
        self.assertFalse(any(key.startswith('MINE_TELEOP_') for key in env))
        for key in ('NODE_OPTIONS', 'LD_PRELOAD', 'DYLD_INSERT_LIBRARIES'):
            self.assertNotIn(key, env)

    def test_native_mode_refuses_an_external_network_interface(self):
        with patch.object(runner.platform, 'system', return_value='Linux'), \
                patch.object(runner.socket, 'if_nameindex', return_value=[(1, 'lo'), (2, 'eth0')]):
            with self.assertRaisesRegex(ValueError, 'only the lo interface'):
                runner.require_loopback_namespace()
        with patch.object(runner.platform, 'system', return_value='Darwin'):
            with self.assertRaisesRegex(ValueError, 'requires Linux'):
                runner.require_loopback_namespace()

    def test_native_mode_accepts_only_loopback_namespace(self):
        with patch.object(runner.platform, 'system', return_value='Linux'), \
                patch.object(runner.socket, 'if_nameindex', return_value=[(1, 'lo')]):
            runner.require_loopback_namespace()

    def test_empty_ctest_inventory_cannot_report_success(self):
        def listing(tests):
            return subprocess.CompletedProcess([], 0, json.dumps({'tests': tests}), '')
        with patch.object(runner.subprocess, 'run', return_value=listing([])):
            with self.assertRaisesRegex(ValueError, 'no registered tests'):
                runner.registered_native_tests(Path('/build'), {})
        with patch.object(runner.subprocess, 'run', return_value=listing([{'name': 'clock-fault-tests'}])):
            self.assertEqual(runner.registered_native_tests(Path('/build'), {}), ['clock-fault-tests'])


if __name__ == '__main__':
    unittest.main()
