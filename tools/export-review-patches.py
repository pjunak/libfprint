#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export the local working delta in review groups, without altering Git's index/history."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GROUPS = ['01-core', '02-goodix', '03-sigfm', '04-build-tests', '05-tools-packaging', '06-docs']


def group(path):
    if path.startswith(('tools/', 'packaging/')):
        return '05-tools-packaging'
    if path.startswith('libfprint/drivers/goodixtls/'):
        return '02-goodix'
    if path.startswith('libfprint/sigfm/') and not path.endswith('meson.build'):
        return '03-sigfm'
    if path.startswith('libfprint/') and not path.endswith('meson.build'):
        return '01-core'
    if path.startswith(('tests/', '.github/', 'data/')) or 'meson' in path or path == '.gitignore':
        return '04-build-tests'
    return '06-docs'


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error('Export outside the source tree')
    output.mkdir(parents=True, exist_ok=True)
    tracked = set(git('diff', '--name-only', '-z', 'HEAD').decode().split('\0')) - {''}
    untracked = set(git('ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')) - {''}
    groups = {name: [] for name in GROUPS}
    for path in sorted(tracked | untracked):
        groups[group(path)].append(path)
    for name, paths in groups.items():
        with (output / (name + '.patch')).open('wb') as file:
            for path in paths:
                if path in tracked:
                    file.write(git('diff', '--binary', '--full-index', 'HEAD', '--', path))
                else:
                    patch = subprocess.run(['git', 'diff', '--no-index', '--binary', '--full-index',
                                            '--', '/dev/null', path], cwd=ROOT, capture_output=True, check=False)
                    if patch.returncode not in (0, 1):
                        raise RuntimeError(patch.stderr.decode())
                    file.write(patch.stdout)
    (output / 'manifest.json').write_text(json.dumps({'base': git('rev-parse', 'HEAD').decode().strip(),
        'groups': groups, 'apply': 'Apply all nonempty patches in lexical order with git apply. '
        'Groups are review units, not independently buildable commits or upstream-ready patches.'}, indent=2) + '\n')
    print(output)
