import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from prepare_release import DESKTOPS, digest, prepare
from publish_release import publish

SHA = 'a' * 40


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'input'
        self.source.mkdir()
        self.output = self.root / 'release'
        for name, (platform, arch) in DESKTOPS.items():
            info = json.dumps({'platform': platform, 'arch': arch, 'source_commit': SHA, 'source_dirty': False, 'version': '1.2.3'})
            self.package(name, 'DESKTOP-BUILD.json', info)
        for role in ['cloud', 'vehicle']:
            self.package(f'mine-teleop-{role}-ubuntu22.04-x64-test.tar.gz', 'BUILD-INFO.txt', f'source_commit={SHA}\nrelease_version=1.2.3\n')

    def package(self, name, member, contents):
        path = self.source / name
        if name.endswith('.zip'):
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('bundle/' + member, contents)
        else:
            with tarfile.open(path, 'w:gz') as archive:
                entry = tarfile.TarInfo('bundle/' + member)
                data = contents.encode()
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        path.with_name(name + '.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest() + '  /old/runner/' + name + '\n')

    def prepare(self, vehicle=True):
        return prepare(self.source, self.output, SHA, '123', vehicle, '1.2.3')

    def test_complete_inventory_and_portable_checksums(self):
        manifest = self.prepare()
        self.assertEqual(len(manifest['packages']), 6)
        self.assertTrue(manifest['includes_vehicle_runtime'])
        for checksum in self.output.glob('*.sha256'):
            self.assertNotIn('/old/runner/', checksum.read_text())

    def test_missing_vehicle_blocks_formal_release(self):
        next(self.source.glob('mine-teleop-vehicle-*.tar.gz')).unlink()
        with self.assertRaisesRegex(ValueError, 'expected one package'):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.assertEqual(len(self.prepare(vehicle=False)['packages']), 5)

    def test_duplicate_package_rejected(self):
        duplicate = self.source / 'duplicate'
        duplicate.mkdir()
        name = next(iter(DESKTOPS))
        (duplicate / name).write_bytes((self.source / name).read_bytes())
        with self.assertRaisesRegex(ValueError, 'expected one package'):
            self.prepare()

    def test_tampered_archive_rejected(self):
        next(self.source.glob('*.zip')).write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            self.prepare()

    def test_wrong_source_rejected_even_with_valid_checksum(self):
        self.package('MineTeleop-win32-x64.zip', 'DESKTOP-BUILD.json', json.dumps({
            'platform': 'win32', 'arch': 'x64', 'source_commit': 'b' * 40, 'source_dirty': False, 'version': '1.2.3'}))
        with self.assertRaisesRegex(ValueError, 'source commit mismatch'):
            self.prepare()

    def test_dirty_source_rejected(self):
        self.package('MineTeleop-win32-x64.zip', 'DESKTOP-BUILD.json', json.dumps({
            'platform': 'win32', 'arch': 'x64', 'source_commit': SHA, 'source_dirty': True, 'version': '1.2.3'}))
        with self.assertRaisesRegex(ValueError, 'dirty source'):
            self.prepare()

    def test_partial_manifest_cannot_publish(self):
        self.prepare(vehicle=False)
        with patch.dict('os.environ', {'GITHUB_SHA': SHA}), patch('publish_release.gh') as gh:
            with self.assertRaisesRegex(ValueError, 'requires all packages'):
                publish(self.output)
            gh.assert_not_called()

    def test_verified_assets_are_published_only_after_upload(self):
        self.prepare()
        env = {'GITHUB_SHA': SHA, 'GITHUB_REPOSITORY': 'owner/repo'}
        tag = type('Result', (), {'returncode': 0, 'stdout': json.dumps({
            'object': {'type': 'commit', 'sha': SHA}})})()
        draft = type('Result', (), {'returncode': 0, 'stdout': json.dumps({
            'draft': True, 'target_commitish': SHA})})()
        assets = [{'name': p.name, 'size': p.stat().st_size, 'digest': 'sha256:' + digest(p)}
                  for p in self.output.iterdir()]
        with patch.dict('os.environ', env), patch('publish_release.subprocess.run', side_effect=[tag, draft]), \
                patch('publish_release.gh', side_effect=['', json.dumps({'assets': assets}), '']) as gh:
            publish(self.output)
            self.assertEqual(gh.call_args_list[-1].args, ('release', 'edit', 'v1.2.3', '--draft=false', '--latest'))

    def test_existing_tag_cannot_be_moved_to_another_commit(self):
        self.prepare()
        env = {'GITHUB_SHA': SHA, 'GITHUB_REPOSITORY': 'owner/repo'}
        result = type('Result', (), {'returncode': 0, 'stdout': json.dumps({
            'object': {'type': 'commit', 'sha': 'b' * 40}})})()
        with patch.dict('os.environ', env), patch('publish_release.subprocess.run', return_value=result), \
                patch('publish_release.gh') as gh:
            with self.assertRaisesRegex(ValueError, 'tag points to another'):
                publish(self.output)
            gh.assert_not_called()

    def test_failed_upload_verification_keeps_draft(self):
        self.prepare()
        env = {'GITHUB_SHA': SHA, 'GITHUB_REPOSITORY': 'owner/repo', 'GITHUB_RUN_NUMBER': '10'}
        tag = type('Result', (), {'returncode': 0, 'stdout': json.dumps({'object': {'type': 'commit', 'sha': SHA}})})()
        result = type('Result', (), {'returncode': 0, 'stdout': json.dumps({'draft': True, 'target_commitish': SHA})})()
        with patch.dict('os.environ', env), patch('publish_release.subprocess.run', side_effect=[tag, result]), \
                patch('publish_release.gh', side_effect=['', json.dumps({'assets': []})]) as gh:
            with self.assertRaisesRegex(ValueError, 'inventory differs'):
                publish(self.output)
            self.assertFalse(any(call.args[:2] == ('release', 'edit') for call in gh.call_args_list))


if __name__ == '__main__':
    unittest.main()
