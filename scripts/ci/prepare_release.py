#!/usr/bin/env python3
"""Validate the complete release inventory before granting a job write access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import zipfile

from release_version import parts

DESKTOPS = {
    'MineTeleop-win32-x64.zip': ('win32', 'x64'),
    'MineTeleop-darwin-arm64.zip': ('darwin', 'arm64'),
    'MineTeleop-win32-arm64.zip': ('win32', 'arm64'),
    'MineTeleop-linux-x64.tar.gz': ('linux', 'x64'),
}


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def metadata(path, filename):
    # Read just the metadata member; never extract untrusted archive paths.
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            matches = [n for n in archive.namelist() if n.endswith('/' + filename)]
            if len(matches) != 1:
                raise ValueError(f'{path.name}: expected one {filename}')
            return archive.read(matches[0]).decode('utf-8')
    with tarfile.open(path, 'r:gz') as archive:
        matches = [m for m in archive.getmembers() if m.name.endswith('/' + filename)]
        if len(matches) != 1 or not matches[0].isfile():
            raise ValueError(f'{path.name}: expected one {filename}')
        return archive.extractfile(matches[0]).read().decode('utf-8')


def prepare(source, destination, commit, run_id, require_vehicle, version):
    parts(version)
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected a full source commit SHA')
    patterns = list(DESKTOPS) + ['mine-teleop-cloud-*.tar.gz']
    if require_vehicle:
        patterns.append('mine-teleop-vehicle-*.tar.gz')
    selected = []
    for pattern in patterns:
        matches = list(source.rglob(pattern))
        if len(matches) != 1:
            raise ValueError(f'{pattern}: expected one package, got {len(matches)}')
        path = matches[0]
        checksum = path.with_name(path.name + '.sha256').read_text().split()[0]
        actual = digest(path)
        if checksum.lower() != actual:
            raise ValueError(f'{path.name}: SHA-256 mismatch')
        if path.name in DESKTOPS:
            info = json.loads(metadata(path, 'DESKTOP-BUILD.json'))
            if (info.get('platform'), info.get('arch')) != DESKTOPS[path.name]:
                raise ValueError(f'{path.name}: platform mismatch')
            if info.get('source_dirty') is not False:
                raise ValueError(f'{path.name}: dirty source')
        else:
            info = dict(line.split('=', 1) for line in metadata(path, 'BUILD-INFO.txt').splitlines() if '=' in line)
        if info.get('version', info.get('release_version')) != version:
            raise ValueError(f'{path.name}: release version mismatch')
        if info.get('source_commit') != commit:
            raise ValueError(f'{path.name}: source commit mismatch')
        selected.append((path, actual))
    destination.mkdir(parents=True, exist_ok=False)
    assets = []
    for path, sha256 in selected:
        suffix = '.tar.gz' if path.name.endswith('.tar.gz') else '.zip'
        name = path.name.removesuffix(suffix) + '-v' + version + suffix
        shutil.copyfile(path, destination / name)
        (destination / (name + '.sha256')).write_text(f'{sha256}  {name}\n')
        assets.append({'name': name, 'sha256': sha256, 'bytes': path.stat().st_size})
    manifest = {'version': version, 'source_commit': commit, 'workflow_run_id': str(run_id),
                'includes_vehicle_runtime': require_vehicle, 'packages': assets}
    (destination / 'RELEASE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--require-vehicle', action='store_true')
    args = parser.parse_args()
    prepare(args.source, args.destination, args.commit, args.run_id, args.require_vehicle, args.version)


if __name__ == '__main__':
    main()
