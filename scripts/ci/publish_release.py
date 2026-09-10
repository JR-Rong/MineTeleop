#!/usr/bin/env python3
"""Publish only a validated draft, leaving existing published releases immutable."""
import json
import os
from pathlib import Path
import subprocess
import sys

from prepare_release import digest
from release_version import parts


def gh(*args):
    return subprocess.check_output(['gh', *args], text=True)


def publish(directory):
    manifest = json.loads((directory / 'RELEASE-MANIFEST.json').read_text())
    commit = os.environ['GITHUB_SHA']
    if manifest['source_commit'] != commit or not manifest['includes_vehicle_runtime']:
        raise ValueError('A formal release requires all packages from the triggering commit')
    repository = os.environ['GITHUB_REPOSITORY']
    parts(manifest['version'])
    tag = 'v' + manifest['version']
    tag_result = subprocess.run(['gh', 'api', f'repos/{repository}/git/ref/tags/{tag}'], capture_output=True, text=True)
    if tag_result.returncode == 0:
        target = json.loads(tag_result.stdout)['object']
        while target['type'] == 'tag':
            target = json.loads(gh('api', f'repos/{repository}/git/tags/{target["sha"]}'))['object']
        if target['type'] != 'commit' or target['sha'] != commit:
            raise ValueError('Existing tag points to another source commit')
    elif 'HTTP 404' not in tag_result.stderr:
        raise RuntimeError(tag_result.stderr)
    endpoint = f'repos/{repository}/releases/tags/{tag}'
    result = subprocess.run(['gh', 'api', endpoint], capture_output=True, text=True)
    if result.returncode == 0:
        release = json.loads(result.stdout)
        if release['target_commitish'] != commit:
            raise ValueError('Existing tag/release belongs to another commit')
        if not release['draft']:
            print(f'Release {tag} is already published; keeping its assets immutable')
            return
    elif 'HTTP 404' in result.stderr:
        notes = (f'Source commit: {commit}\n\n'
                 f'Build and validation: https://github.com/{repository}/actions/runs/{os.environ["GITHUB_RUN_ID"]}\n\n'
                 'Includes Windows arm64/x64, macOS arm64 and Ubuntu x64 desktop controllers, '
                 'Ubuntu 22.04 x64 cloud and vehicle bundles. SHA-256 files and RELEASE-MANIFEST.json '
                 'identify all packages. CI validation does not replace target hardware acceptance.\n')
        gh('release', 'create', tag, '--target', commit, '--draft', '--title', f'MineTeleop {tag}', '--notes', notes)
    else:
        raise RuntimeError(result.stderr)
    assets = sorted(directory.iterdir())
    gh('release', 'upload', tag, *(str(p) for p in assets), '--clobber')
    release = json.loads(gh('api', endpoint))
    remote = {a['name']: a for a in release['assets']}
    if set(remote) != {p.name for p in assets}:
        raise ValueError('Draft asset inventory differs from validated packages')
    for path in assets:
        asset = remote[path.name]
        if asset['size'] != path.stat().st_size or asset.get('digest') != 'sha256:' + digest(path):
            raise ValueError(f'Uploaded asset verification failed: {path.name}')
    gh('release', 'edit', tag, '--draft=false', '--latest')
    print(f'https://github.com/{repository}/releases/tag/{tag}')


if __name__ == '__main__':
    publish(Path(sys.argv[1]))
