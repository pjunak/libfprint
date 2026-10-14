#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Install, check or remove the reader key Windows paired the sensor with.

Windows (Windows Hello) pairs the 27c6:55a2 reader with its own random 32-byte
key. With that key installed, the driver talks to the reader the way Windows
does: Windows Hello keeps working, and the USB link is protected by a secret
key instead of the public reference key. This tool does not extract the key;
obtain it from your Windows installation yourself.

`install` reads the key from FILE or standard input, never from the command
line (which other users can see): 64 hexadecimal digits, optionally separated
by spaces or colons, or the 32 raw bytes. It writes the key, readable only by
root, to /etc/libfprint-goodix/windows-psk. fprintd reads it at the next
fingerprint attempt; no restart is needed.

Once a key is installed, a reader that reports the reference key is refused,
so a device posing as the reader cannot fall back to the public key. After
restoring the reference key on purpose, run `remove`.
"""
import argparse
import getpass
import os
from pathlib import Path
import re
import stat
import sys

# The driver reads this path and format (goodix55x4.c, goodix55x4_load_windows_psk).
KEY_FILE = 'etc/libfprint-goodix/windows-psk'
KEY_SIZE = 32
# The longest input worth parsing: hex digits with a separator after each byte.
MAX_INPUT = 4096


class KeyFileError(Exception):
    pass


def parse_key(data, allow_raw=True):
    """The key in data: hex text (separators and a psk= or 0x prefix allowed) or raw bytes."""
    text = re.sub(rb'^(psk=)?(0x)?', b'', re.sub(rb'[\s:]', b'', data), flags=re.I)
    if text and re.fullmatch(rb'[0-9a-fA-F]+', text):
        # Anything that looks like hex text must be a whole key, never raw bytes.
        if len(text) != 2 * KEY_SIZE:
            raise KeyFileError(f'expected {2 * KEY_SIZE} hexadecimal digits, got {len(text)}')
        key = bytes.fromhex(text.decode('ascii'))
    elif allow_raw and len(data) == KEY_SIZE:
        key = data
    else:
        raise KeyFileError(f'expected {2 * KEY_SIZE} hexadecimal digits'
                           + (f' or {KEY_SIZE} raw bytes' if allow_raw else ''))
    if not any(key):
        raise KeyFileError('that is the public reference key, which the driver uses without a key file')
    return key


def file_text(key):
    return f'psk={key.hex()}\n'


def read_key(source):
    if source is not None and source != '-':
        with open(source, 'rb') as file:
            return parse_key(file.read(MAX_INPUT + 1))
    if sys.stdin.isatty():
        typed = getpass.getpass(f'Key ({2 * KEY_SIZE} hexadecimal digits): ')
        return parse_key(typed.encode('ascii', 'replace'), allow_raw=False)
    return parse_key(sys.stdin.buffer.read(MAX_INPUT + 1))


def inspect(path, owner):
    """('missing' | 'unknown' | 'unreadable' | 'invalid' | 'valid', detail): what the
    driver will make of path. 'unknown' and 'unreadable' mean only root can tell.

    The driver runs as owner (root). It refuses an 'invalid' file and, because
    anything at the path counts as an installed key, the reader with it.
    """
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return 'missing', None
    except PermissionError:
        return 'unknown', None
    except OSError as error:
        return 'invalid', f'{path} cannot be checked: {error.strerror}'
    if not stat.S_ISREG(info.st_mode):
        return 'invalid', f'{path} is not a regular file'
    if info.st_nlink != 1:
        return 'invalid', f'{path} has more than one name (hard link)'
    if info.st_uid != owner:
        return 'invalid', f'{path} is not owned by root'
    if info.st_mode & 0o077:
        return 'invalid', f'{path} is accessible to other users (mode {stat.S_IMODE(info.st_mode):04o})'
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    except PermissionError:
        return 'unreadable', None
    except OSError as error:
        return 'invalid', f'{path} cannot be read: {error.strerror}'
    with os.fdopen(descriptor, 'rb') as file:
        data = file.read(MAX_INPUT + 1)
    match = re.fullmatch(rb'psk=([0-9a-fA-F]{%d})\n' % (2 * KEY_SIZE), data)
    if not match:
        return 'invalid', f'{path} is malformed'
    return 'valid', bytes.fromhex(match.group(1).decode('ascii'))


def key_owner(root):
    """fprintd runs as root; a fixture belongs to whoever runs the tests."""
    return 0 if root == Path('/') else os.geteuid()


def require_root(root):
    if root == Path('/') and os.geteuid() != 0:
        raise KeyFileError('changing /etc/libfprint-goodix needs root; run with sudo')


def write_key(path, key):
    """Replace path atomically with a new private file, never writing through an existing one."""
    # Others may see that a key exists (goodix-diagnose reports it), not read it.
    path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temporary = path.with_name(f'.{path.name}.tmp')
    try:
        temporary.unlink()  # left over from an interrupted run
    except FileNotFoundError:
        pass
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        with os.fdopen(descriptor, 'w') as out:
            os.fchmod(out.fileno(), 0o600)
            out.write(file_text(key))
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def install(root, source, out):
    require_root(root)
    key = read_key(source)
    path = root / KEY_FILE
    state, current = inspect(path, key_owner(root))
    if state == 'valid' and current == key:
        print(f'{path}: this key is already installed', file=out)
        return
    if path.is_dir() and not path.is_symlink():
        raise KeyFileError(f'{path} is a directory; remove it by hand')
    write_key(path, key)
    print(f'{path}: key {"replaced" if state != "missing" else "installed"}', file=out)
    print('fprintd uses it from the next fingerprint attempt; test with "fprintd-verify".\n'
          'From now on a reader that reports the public reference key is refused. If the\n'
          'attempt fails and "journalctl -u fprintd" shows "TLS with the key ... failed",\n'
          'the key is not the reader\'s: Windows may have paired it again since.', file=out)


def remove(root, out):
    require_root(root)
    path = root / KEY_FILE
    try:
        path.unlink()
    except FileNotFoundError:
        print(f'{path}: no key installed', file=out)
        return
    except IsADirectoryError:
        raise KeyFileError(f'{path} is a directory; remove it by hand')
    print(f'{path}: removed. The driver now expects the reader to be paired with the public\n'
          'reference key; a reader still paired by Windows reports "Invalid device PSK".', file=out)


def status(root, out):
    """Print what the driver will do with the installed key; False if it will refuse it."""
    path = root / KEY_FILE
    state, detail = inspect(path, key_owner(root))
    if state == 'missing':
        print('no key installed: the reader must be paired with the public reference key', file=out)
    elif state == 'unknown':
        print(f'cannot check {path}; run with sudo', file=out)
    elif state == 'unreadable':
        print(f'key installed in {path} (run with sudo to check it)', file=out)
    elif state == 'valid':
        print(f'key installed in {path}: the reader must be paired with it', file=out)
    else:
        print(f'warning: {detail}. The driver refuses the reader until this is fixed: '
              'run "sudo goodix-windows-key install" again, or "sudo goodix-windows-key remove"', file=out)
        return False
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('status', help='show whether a key is installed and usable')
    installer = commands.add_parser('install', help='install or replace the key')
    installer.add_argument('file', nargs='?', metavar='FILE',
                           help='file holding the key; "-" or none reads standard input')
    commands.add_parser('remove', help='remove the key; the reader must use the reference key again')
    parser.add_argument('--root', type=Path, default=Path('/'), help='operate on a filesystem fixture')
    args = parser.parse_args(argv)
    try:
        if args.command == 'status':
            return 0 if status(args.root, sys.stdout) else 1
        if args.command == 'install':
            install(args.root, args.file, sys.stdout)
        else:
            remove(args.root, sys.stdout)
    except (KeyFileError, OSError) as error:
        print(f'goodix-windows-key: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
