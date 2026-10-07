#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile C and C++ consumers against staged installed headers."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='libfprint-headers-') as temp:
        root = Path(temp)
        subprocess.run(['meson', 'install', '-C', str(args.build), '--no-rebuild', '--destdir', temp], check=True)
        header = next(root.rglob('fprint.h'))
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'gio-2.0'], text=True))
        for lang, compiler in [('c', os.environ.get('CC', 'cc')), ('cpp', os.environ.get('CXX', 'c++'))]:
            source = root / f'consumer.{lang}'
            source.write_text('#include <fprint.h>\nint main(void) { FpContext *p = 0; return p != 0; }\n')
            subprocess.run([*shlex.split(compiler), '-Werror', '-fsyntax-only',
                            '-I' + str(header.parent), *flags, str(source)], check=True)
