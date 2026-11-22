"""Check deployment arguments without building images or starting host services."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DockerControlEntryTests(unittest.TestCase):
    def run_script(self, platform):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / 'uname').write_text('#!/bin/sh\necho ' + platform + '\n')
            (d / 'docker').write_text('''#!/usr/bin/env python3
import json,os,sys
with open(os.environ['DOCKER_CALLS'],'a') as f: f.write(json.dumps(sys.argv[1:])+'\\n')
if sys.argv[1]=='inspect': print('false')
''')
            for name in ('uname', 'docker'):
                (d / name).chmod(0o755)
            env = dict(os.environ, PATH=str(d) + os.pathsep + os.environ['PATH'], DOCKER_CALLS=str(d/'calls'),
                       MINE_TELEOP_CONTROL_PLANE_CONSOLE_PORT='18080', MINE_TELEOP_CONTROL_PLANE_SIGNALING_PORT='18765')
            result = subprocess.run(['bash', str(ROOT/'scripts/deploy/run_control_plane_docker.sh')],
                                    env=env, capture_output=True, text=True, timeout=10)
            calls = [json.loads(line) for line in (d/'calls').read_text().splitlines()] if (d/'calls').exists() else []
            return result, calls

    def test_linux_uses_host_loopback_and_custom_ports(self):
        result, calls = self.run_script('Linux')
        self.assertEqual(result.returncode, 1)  # Mock inspect ends the monitor.
        runs = [c for c in calls if c[0]=='run']
        self.assertEqual(len(runs), 2)
        for run, port in zip(runs, ('18765', '18080')):
            self.assertEqual(run[run.index('--network')+1], 'host')
            self.assertEqual(run[run.index('--host')+1], '127.0.0.1')
            self.assertEqual(run[run.index('--port')+1], port)
            self.assertNotIn('-p', run)
        urls = [c[c.index('--url')+1] for c in calls if '--url' in c]
        self.assertEqual(set(urls), {'http://127.0.0.1:18765/health', 'http://127.0.0.1:18080/health'})
        self.assertIn('DRIVER_CONSOLE_URL=http://127.0.0.1:18080', result.stdout)

    def test_desktop_rejects_before_docker_or_cleanup(self):
        for platform in ('Darwin', 'MINGW64_NT'):
            result, calls = self.run_script(platform)
            self.assertEqual(result.returncode, 2)
            self.assertIn('native desktop client', result.stderr)
            self.assertEqual(calls, [])

if __name__ == '__main__':
    unittest.main()
