#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Enable, inspect or remove fingerprint authentication for sudo, polkit and login.

Each managed file is the distribution's current PAM policy with one marked block
inserted before its password stack, so removal deletes exactly what was added.
Files replaced by `install` are backed up under /var/lib/goodix-fingerprint-pam.
The KDE lock screen already uses the distribution's kde-fingerprint service.

`install` and `diff` default to sudo and polkit-1. The login screen (plasmalogin)
is opt-in: a fingerprint login cannot unlock KWallet or GNOME Keyring, which
then ask for the password separately.
"""
import argparse
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys
import time

BEGIN = '# BEGIN goodix-fingerprint: managed by goodix-fingerprint-pam; remove with "uninstall"'
END = '# END goodix-fingerprint'

# One prompt for both password and finger (pam/pam_fprint_parallel.c). It
# returns success only for a fingerprint match; a typed password is passed
# to pam_unix in the included stack, which checks it.
PARALLEL = '/opt/libfprint-goodix/lib/security/pam_fprint_parallel.so'
# A match continues to the next line (faillock authsucc, which ends the stack);
# a typed password (PAM_IGNORE), a missing module or any error jumps over it to
# the password stack; only a cancelled prompt aborts. Plain `sufficient` would
# leave faillock's counter untouched by fingerprint logins, and default=die
# would turn a missing module into a sudo lockout.
PARALLEL_CONTROL = '[success=ok ignore=1 conv_err=die default=1]'
FAILLOCK_RESET = 'auth        [default=done]  pam_faillock.so authsucc'

# The fingerprint step must come after the account checks it would otherwise
# bypass (shell, nologin, faillock) and before the password stack.
SERVICES = {
    # Plasma Login Manager: a typed password is checked first, so it logs in
    # at once and unlocks KWallet. An empty (or wrong) password falls through
    # to a fingerprint attempt. pam_unix's own failure delay applies only if
    # the whole stack fails.
    'plasmalogin': (r'auth\s+include\s+system-login\b', [
        'auth        required    pam_shells.so',
        'auth        requisite   pam_nologin.so',
        'auth        requisite   pam_faillock.so preauth',
        'auth        [success=2 new_authtok_reqd=2 default=ignore]  pam_unix.so try_first_pass nullok',
        '-auth       [success=ok default=1]  pam_fprintd.so max-tries=2 timeout=15',
        FAILLOCK_RESET,
    ]),
    # Terminal: type the password or swipe, no timeout. Skipped over SSH and
    # for piped input (sudo -S, pipelines), where sudo's own prompt follows.
    'sudo': (r'auth\s+include\s+system-auth\b', [
        'auth        requisite   pam_faillock.so preauth',
        f'-auth       {PARALLEL_CONTROL}  {PARALLEL} mode=tty',
        FAILLOCK_RESET,
    ]),
    # pkexec, run0 and graphical administrator dialogs: same, through the agent.
    'polkit-1': (r'auth\s+include\s+system-auth\b', [
        'auth        requisite   pam_faillock.so preauth',
        f'-auth       {PARALLEL_CONTROL}  {PARALLEL} mode=conv',
        FAILLOCK_RESET,
    ]),
}

# Typing the password at the login screen unlocks the wallets; the fingerprint
# covers everything after that (lock screen, sudo, polkit).
DEFAULT_INSTALL = ('sudo', 'polkit-1')

FPRINTD = 'usr/lib/fprintd'
PACKAGE_LIBRARY = '/opt/libfprint-goodix/lib'
DROPIN_DIR = 'etc/systemd/system/fprintd.service.d'
STATE_DIR = 'var/lib/goodix-fingerprint-pam'


class PolicyError(Exception):
    pass


def read(path):
    try:
        return path.read_text()
    except FileNotFoundError:
        return None


def strip_block(text):
    """Remove every managed block, leaving the surrounding policy untouched."""
    lines, inside = [], False
    for line in text.splitlines(keepends=True):
        if line.rstrip('\n') == BEGIN:
            inside = True
        elif line.rstrip('\n') == END and inside:
            inside = False
        elif not inside:
            lines.append(line)
    if inside:
        raise PolicyError('unterminated goodix-fingerprint block')
    return ''.join(lines)


def paths(root, name):
    return root / 'etc/pam.d' / name, root / 'usr/lib/pam.d' / name


def base_policy(root, name):
    """The distribution policy the managed file is generated from."""
    local, vendor = paths(root, name)
    text = read(vendor)
    if text is not None:
        return text, vendor
    text = read(local)
    if text is not None:
        return strip_block(text), local
    raise PolicyError(f'no PAM policy for {name} in {local.parent} or {vendor.parent}')


def render(name, base):
    anchor, block = SERVICES[name]
    lines = base.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if re.match(anchor, line.strip()):
            managed = [BEGIN + '\n'] + [entry + '\n' for entry in block] + [END + '\n']
            return ''.join(lines[:index] + managed + lines[index:])
    raise PolicyError(f'{name}: no "{anchor}" line to insert the fingerprint step before; '
                      'the distribution policy changed and needs a manual review')


def inspect(root, name):
    local, vendor = paths(root, name)
    current = read(local)
    report = {'name': name, 'file': local, 'current': current, 'managed': bool(current and BEGIN in current),
              'pacnew': local.with_name(name + '.pacnew').exists()}
    try:
        base, source = base_policy(root, name)
        report['desired'] = render(name, base)
        report['source'] = source
        report['local_changes'] = (current is not None and source == vendor and
                                   strip_block(current) != base)
    except PolicyError as error:
        report['error'] = str(error)
    effective = current if current is not None else read(vendor)
    report['fingerprint'] = bool(effective and re.search(r'^\s*-?auth\s.*pam_fprint(d|_parallel)\.so', effective, re.M))
    return report


def library_dropin(root):
    """The library directory fprintd's drop-ins put first, and the drop-in
    that sets it: systemd applies them in name order, so the last one wins."""
    selected = None, None
    for conf in sorted((root / DROPIN_DIR).glob('*.conf')):
        for line in (read(conf) or '').splitlines():
            match = re.match(r'\s*Environment=.*\bLD_LIBRARY_PATH=([^\s"]+)', line)
            if match:
                selected = match.group(1).split(':')[0], conf.name
    return selected


def selected_library(root):
    return library_dropin(root)[0]


def library_problems(root):
    library, conf = library_dropin(root)
    if library is None:
        return ['fprintd has no LD_LIBRARY_PATH drop-in; the distribution libfprint has no 55a2 driver']
    if not (root / library.lstrip('/') / 'libfprint-2.so.2').exists():
        return [f'fprintd drop-in selects {library}, which has no libfprint-2.so.2']
    # An older setup (such as Ravira43/libfprint's override.conf) sorts after
    # this package's drop-in and keeps fprintd on the old library.
    package = root / PACKAGE_LIBRARY.lstrip('/') / 'libfprint-2.so.2'
    if package.exists() and library.rstrip('/') != PACKAGE_LIBRARY:
        return [f'fprintd uses {library} (set in {DROPIN_DIR}/{conf}), not the installed package\'s '
                f'{PACKAGE_LIBRARY}: remove or fix that drop-in, then "systemctl daemon-reload"']
    if root != Path('/') or not (root / FPRINTD).exists():
        return []
    try:
        result = subprocess.run(['ldd', '-r', '/' + FPRINTD], capture_output=True, text=True,
                                timeout=20, env={**os.environ, 'LD_LIBRARY_PATH': library})
    except (OSError, subprocess.TimeoutExpired) as error:
        return [f'could not check fprintd against {library}: {error}']
    output = result.stdout + result.stderr
    problems = [line.strip() for line in output.splitlines()
                if 'undefined symbol' in line or 'not found' in line]
    if not problems and f'{library.rstrip("/")}/libfprint-2.so.2' not in output:
        problems.append(f'fprintd does not resolve libfprint-2.so.2 from {library}')
    if problems:
        problems.insert(0, f'fprintd cannot run with {library} (rebuild libfprint-goodix-local '
                           'against the new fprintd):')
    return problems


def diff(name, old, new):
    return ''.join(difflib.unified_diff((old or '').splitlines(keepends=True), new.splitlines(keepends=True),
                                        f'a/etc/pam.d/{name}', f'b/etc/pam.d/{name}'))


def write_private(path, text, mode):
    """Create path afresh (never through an existing file or symlink)."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    with os.fdopen(descriptor, 'w') as out:
        out.write(text)
        os.fchmod(out.fileno(), mode)


def write_atomic(path, text):
    temporary = path.with_name(f'.{path.name}.goodix-tmp')
    try:
        temporary.unlink()  # left over from an interrupted run
    except FileNotFoundError:
        pass
    write_private(temporary, text, 0o644)
    os.replace(temporary, path)


def require_root(root):
    if root == Path('/') and os.geteuid() != 0:
        raise PolicyError('changing /etc/pam.d needs root; run with sudo')


def install(root, names, out):
    require_root(root)
    reports = [inspect(root, name) for name in names]
    failed = [report['error'] for report in reports if 'error' in report]
    if failed:
        raise PolicyError('; '.join(failed))  # change nothing if any policy is unexpected
    backups = root / STATE_DIR / 'backup'
    stamp = time.strftime('%Y%m%dT%H%M%S')
    for report in reports:
        name, current, desired = report['name'], report['current'], report['desired']
        if current == desired:
            print(f'{name}: already enabled', file=out)
            continue
        if current is not None:
            backups.mkdir(parents=True, exist_ok=True, mode=0o700)
            write_private(backups / f'{name}.{stamp}', current, 0o600)
        if report['local_changes']:
            print(f'{name}: replacing local changes in {report["file"]} '
                  f'(backup: {backups / f"{name}.{stamp}"})', file=out)
        print(diff(name, current, desired), end='', file=out)
        report['file'].parent.mkdir(parents=True, exist_ok=True)
        write_atomic(report['file'], desired)
        print(f'{name}: fingerprint enabled', file=out)


def uninstall(root, names, out):
    require_root(root)
    for name in names:
        local, vendor = paths(root, name)
        current = read(local)
        if current is None or BEGIN not in current:
            print(f'{name}: not managed, left unchanged', file=out)
            continue
        remaining = strip_block(current)
        if remaining == read(vendor):
            local.unlink()  # identical to the distribution policy again
            print(f'{name}: removed {local}; the distribution policy applies', file=out)
        else:
            print(diff(name, current, remaining), end='', file=out)
            write_atomic(local, remaining)
            print(f'{name}: fingerprint step removed', file=out)


def problems(root, names):
    found = []
    for name in names:
        report = inspect(root, name)
        if not report['managed']:
            continue
        if 'error' in report:
            found.append(report['error'])
        elif report['current'] != report['desired']:
            if strip_block(report['current']) == strip_block(report['desired']):
                found.append(f'{name}: this version of goodix-fingerprint-pam sets it up differently; '
                             f'run "sudo goodix-fingerprint-pam install {name}" to update')
            else:
                found.append(f'{name}: the distribution changed {report["source"]}; '
                             f'review and run "sudo goodix-fingerprint-pam install {name}" to regenerate')
        if PARALLEL in (report['current'] or '') and not (root / PARALLEL.lstrip('/')).exists():
            found.append(f'{name}: {PARALLEL} is missing; reinstall libfprint-goodix-local '
                         f'or run "sudo goodix-fingerprint-pam uninstall {name}"')
        if report['pacnew']:
            found.append(f'{name}: merge {report["file"]}.pacnew (the fingerprint block stays in '
                         f'{report["file"]}), then run "sudo goodix-fingerprint-pam check"')
    return found + library_problems(root)


def status(root, names, out):
    for name in names:
        report = inspect(root, name)
        state = 'fingerprint' if report['fingerprint'] else 'password only'
        if report['managed']:
            state += ', managed' + ('' if report['current'] == report.get('desired') else ', OUT OF DATE')
        elif report['current'] is not None and paths(root, name)[1].exists():
            state += f', unmanaged local override {report["file"]}'
        print(f'{name:16} {state}', file=out)
    print(f'{"kde-fingerprint":16} fingerprint (lock screen; distribution policy)', file=out)
    print(f'fprintd library: {selected_library(root) or "distribution default"}', file=out)
    for problem in problems(root, names):
        print(f'warning: {problem}', file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('status', 'diff', 'install', 'uninstall', 'check'))
    parser.add_argument('services', nargs='*', metavar='SERVICE',
                        help=f'any of {", ".join(SERVICES)} (install/diff default: '
                             f'{", ".join(DEFAULT_INSTALL)}; other commands: all)')
    parser.add_argument('--root', type=Path, default=Path('/'), help='operate on a filesystem fixture')
    args = parser.parse_args(argv)
    unknown = sorted(set(args.services) - set(SERVICES))
    if unknown:
        parser.error(f'unknown service {", ".join(unknown)}; choose from {", ".join(SERVICES)}')
    default = DEFAULT_INSTALL if args.command in ('install', 'diff') else SERVICES
    names = args.services or list(default)
    try:
        if args.command == 'status':
            status(args.root, names, sys.stdout)
        elif args.command == 'diff':
            for name in names:
                report = inspect(args.root, name)
                if 'error' in report:
                    raise PolicyError(report['error'])
                print(diff(name, report['current'], report['desired']), end='')
        elif args.command == 'install':
            install(args.root, names, sys.stdout)
        elif args.command == 'uninstall':
            uninstall(args.root, names, sys.stdout)
        else:
            found = problems(args.root, names)
            for problem in found:
                print(f'goodix-fingerprint-pam: {problem}', file=sys.stderr)
            return 1 if found else 0
    except (PolicyError, OSError) as error:
        print(f'goodix-fingerprint-pam: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
