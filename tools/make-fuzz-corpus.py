#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Create small public, synthetic seeds; fuzzing never needs real fingerprints."""
import argparse
from pathlib import Path

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    for name in ('goodix', 'print'):
        (args.output / name).mkdir(parents=True, exist_ok=True)
    seeds = {'empty-length': bytes.fromhex('a8000088'),
             'ack': bytes.fromhex('b00300a8014e'),
             'outer': bytes.fromhex('a00400a4a8010001'),
             'maximum-header': bytes.fromhex('a0ffff9e'),
             'padding': bytes(64)}
    for name, data in seeds.items():
        (args.output / 'goodix' / name).write_bytes(data)
    for index, data in enumerate((b'FP3', b'FP3\x00', b'FP3' + bytes(64))):
        (args.output / 'print' / str(index)).write_bytes(data)
