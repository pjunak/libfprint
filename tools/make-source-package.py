#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate a deterministic source archive and checksum-pinned Arch PKGBUILD."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def build(output):
    output = output.resolve()
    if output == ROOT or ROOT in output.parents:
        raise ValueError('Use an output directory outside the source tree.')
    output.mkdir(parents=True, exist_ok=True)
    names = sorted(set(git('ls-files', '-z', '--cached', '--others', '--exclude-standard').decode().split('\0')) - {''})
    entries = []
    digest = hashlib.sha256()
    for name in names:
        path = ROOT / name
        if not path.exists():
            continue  # Locally removed tracked file.
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Unsupported source entry: {name}')
        mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
        data = path.read_bytes()
        digest.update(name.encode() + b'\0' + str(mode).encode() + b'\0' + str(len(data)).encode() + b'\0' + data)
        entries.append((name, mode, data))
    revision = git('rev-parse', 'HEAD').decode().strip()
    identity = revision[:12] + '.' + digest.hexdigest()[:16]
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', git('show', '-s', '--format=%ct', 'HEAD').decode().strip()))
    manifest = json.dumps({'revision': revision, 'source_sha256': digest.hexdigest(), 'build_id': identity}, sort_keys=True).encode()
    entries.append(('source-info.json', 0o644, manifest + b'\n'))
    archive = output / 'libfprint-goodix-source.tar.gz'
    with archive.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=epoch) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as tar:
            for name, mode, data in sorted(entries):
                info = tarfile.TarInfo('libfprint-goodix/' + name)
                info.size, info.mode, info.mtime = len(data), mode, epoch
                tar.addfile(info, io.BytesIO(data))
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    template = (ROOT / 'packaging/arch/PKGBUILD.in').read_text()
    version = '1.94.6.r' + git('rev-list', '--count', 'HEAD').decode().strip() + '.g' + identity
    recipe = template.replace('@SHA256@', sha).replace('@BUILD_ID@', identity).replace('@PKGVER@', version)
    (output / 'PKGBUILD').write_text(recipe)
    print(json.dumps({'archive': str(archive), 'sha256': sha, 'build_id': identity, 'pkgver': version}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.output)
