#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read-only fingerprint diagnostics. Does not claim USB or read templates/images."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def command(*args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=10, check=False)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def pam_chain(service, roots, seen=None):
    """Follow only PAM auth includes/substacks; account/session rules cannot enable auth."""
    seen = set() if seen is None else seen
    if not re.fullmatch(r"[\w.-]+", service) or service in seen:
        return []
    seen.add(service)
    source = next((root / service for root in roots if (root / service).is_file()), None)
    if source is None:
        return [{"service": service, "status": "missing"}]
    try:
        lines = source.read_text().splitlines()
    except OSError:
        return [{"service": service, "status": "unreadable"}]
    result = []
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("@include "):
            result.extend(pam_chain(line.split()[1], roots, seen))
            continue
        match = re.match(r"-?auth\s+(\[[^]]+\]|\S+)\s+(\S+)", line)
        if not match:
            continue
        control, module = match.groups()
        if control in ("include", "substack"):
            result.extend(pam_chain(module, roots, seen))
        else:
            result.append({"service": service, "file": str(source), "control": control,
                           "module": module})
    return result


def firmware_from_journal(journal):
    """Use observed firmware, including errors, never the list of allowed versions."""
    firmware = re.findall(r'''(?:Device firmware: "|Unsupported Goodix firmware ')([A-Za-z0-9_]+)["']''',
                          journal)
    return firmware[-1] if firmware else None


def collect(root=Path('/')):
    devices = []
    for path in sorted((root / 'sys/bus/usb/devices').glob('*')):
        try:
            vid = (path / 'idVendor').read_text().strip()
            pid = (path / 'idProduct').read_text().strip()
            if vid == '27c6':
                devices.append({'usb_id': f'{vid}:{pid}', 'sysfs': str(path),
                                'profile': '55a2' if pid == '55a2' else 'unsupported',
                                'supported_firmware': ['GF3206_RTSEC_APP_10052', 'GF3206_RTSEC_APP_10062']
                                if pid == '55a2' else []})
        except OSError:
            continue
    pam_roots = [root / 'etc/pam.d', root / 'usr/lib/pam.d']
    services = {name: pam_chain(name, pam_roots) for name in
                ('plasmalogin', 'sddm', 'gdm-fingerprint', 'kde-fingerprint', 'sudo', 'polkit-1')}
    report = {'devices': devices, 'pam_auth': services,
              'fingerprint_module_present': {k: any(Path(x.get('module', '')).name == 'pam_fprintd.so'
                                                     for x in v) for k, v in services.items()},
              'note': 'Module presence does not prove PAM ordering or successful authentication.'}
    if root != Path('/'):
        return report
    report['display_manager'] = command('systemctl', 'show', 'display-manager.service',
                                        '--property=Id', '--value')
    report['fprintd_status'] = command('systemctl', 'show', 'fprintd.service',
                                      '--property=ActiveState,SubState,FragmentPath,DropInPaths')
    pid = command('systemctl', 'show', 'fprintd.service', '--property=MainPID', '--value')
    report['loaded_libraries'] = None
    if pid and pid.isdecimal() and pid != '0':
        try:
            report['loaded_libraries'] = sorted({line.split(maxsplit=5)[-1]
                for line in Path(f'/proc/{pid}/maps').read_text().splitlines()
                if '/libfprint' in line and len(line.split(maxsplit=5)) == 6})
        except OSError:
            pass
    journal = command('journalctl', '-b', '-u', 'fprintd.service', '-n', '500', '--no-pager', '-o', 'cat')
    # Extract only firmware/state identifiers, never arbitrary debug payloads.
    report['last_firmware'] = None
    report['last_command'] = None
    if journal:
        commands = re.findall(r'Running command: (0x[0-9a-fA-F]{2})', journal)
        report['last_firmware'] = firmware_from_journal(journal)
        report['last_command'] = commands[-1] if commands else None
    report['build_manifests'] = {}
    for prefix in ('/opt/libfprint-goodix', '/opt/fprint55a2', '/usr'):
        path = Path(prefix) / 'share/libfprint-goodix/build-info.json'
        try:
            report['build_manifests'][str(path)] = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/'), help='Inspect a filesystem fixture offline')
    args = parser.parse_args()
    print(json.dumps(collect(args.root), indent=2))


if __name__ == '__main__':
    main()
