#!/usr/bin/env python3
"""Plan A.B.C from published ancestor releases and the PR/commit change types."""
import json
import os
from pathlib import Path
import re
import subprocess

VERSION = re.compile(r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')
LEVELS = {'patch': 0, 'minor': 1, 'major': 2}


def parts(version):
    if not VERSION.fullmatch(version):
        raise ValueError(f'Invalid A.B.C version: {version}')
    return tuple(map(int, version.split('.')))


def classify(title, labels=()):
    level = 0
    match = re.match(r'^(\w+)(?:\([^)]*\))?(!)?:', title, re.I)
    if match:
        kind, breaking = match.groups()
        level = 2 if breaking or kind.lower() in ('refactor', 'refact') else (1 if kind.lower() == 'feat' else 0)
    for label in labels:
        if label.startswith('release:'):
            name = label.removeprefix('release:')
            if name not in LEVELS:
                raise ValueError(f'Unknown release label: {label}')
            level = max(level, LEVELS[name])
    return level


def bump(version, level):
    result = list(parts(version))
    index = 2 - level
    result[index] += 1
    for i in range(index + 1, 3):
        result[i] = 0
    return '.'.join(map(str, result))


def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()


def api(endpoint):
    return json.loads(subprocess.check_output(['gh', 'api', endpoint, '--paginate', '--slurp'], text=True))


def plan():
    repo = os.environ['GITHUB_REPOSITORY']
    head = git('rev-parse', 'HEAD')
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    releases = [r for page in api(f'repos/{repo}/releases?per_page=100') for r in page
                if not r['draft'] and not r['prerelease'] and VERSION.fullmatch(r['tag_name'].removeprefix('v'))]
    ancestors = []
    for release in releases:
        tag = release['tag_name']
        if subprocess.run(['git', 'merge-base', '--is-ancestor', f'refs/tags/{tag}', head], capture_output=True).returncode == 0:
            ancestors.append(release)
    previous = max(ancestors, key=lambda r: parts(r['tag_name'].removeprefix('v')), default=None)
    base = previous['tag_name'].removeprefix('v') if previous else json.loads(Path('desktop/package.json').read_text())['version']
    if previous and git('rev-list', '-n', '1', previous['tag_name']) == head:
        return {'version': base, 'base_version': base, 'bump': 'none', 'source_commit': head}
    level = 0
    pr = event.get('pull_request')
    if pr:
        level = classify(pr['title'], [label['name'] for label in pr['labels']])
    else:
        start = previous['tag_name'] if previous else event.get('before')
        revision = f'{start}..{head}' if start and set(start) != {'0'} else head
        commits = git('log', '--format=%H', revision).splitlines()
        seen = set()
        for commit in commits:
            level = max(level, classify(git('show', '-s', '--format=%s', commit)))
            for page in api(f'repos/{repo}/commits/{commit}/pulls?per_page=100'):
                for item in page:
                    if item['number'] in seen or not item['merged_at'] or item['base']['ref'] != 'main':
                        continue
                    seen.add(item['number'])
                    level = max(level, classify(item['title'], [label['name'] for label in item['labels']]))
    return {'version': bump(base, level), 'base_version': base,
            'bump': next(name for name, value in LEVELS.items() if value == level), 'source_commit': head}


if __name__ == '__main__':
    result = plan()
    print(json.dumps(result, indent=2))
    with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
        for key, value in result.items():
            stream.write(f'{key}={value}\n')
