#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Import the cleartext setup of a Linux usbmon capture for the Goodix replay test.

Recorded TLS is deliberately excluded. The test completes the conversation with
a live TLS peer and synthetic image data, without changing production randomness.
"""
import argparse
import configparser
import csv
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

FIELDS = ('usb.bus_id', 'usb.device_address', 'usb.endpoint_address',
          'usb.urb_type', 'usb.urb_status', 'usb.data_len', 'usb.capdata')
COMMANDS = (0x00, 0x96, 0x00, 0xa8, 0xe4, 0xa2, 0x70, 0x90)
MAX_PACKET = 65536 + 64


def protocol_packets(data):
    """Decode complete padded messages, rejecting truncation and bad checksums."""
    packets = []
    while data:
        data = data.lstrip(b'\0')
        if not data:
            break
        if len(data) < 4 or data[0] != 0xa0 or sum(data[:3]) & 255 != data[3]:
            raise ValueError('Invalid cleartext packet header')
        length = int.from_bytes(data[1:3], 'little')
        if length < 4 or len(data) < length + 4:
            raise ValueError('Truncated cleartext packet')
        body, data = data[4:4 + length], data[4 + length:]
        if int.from_bytes(body[1:3], 'little') != len(body) - 3:
            raise ValueError('Invalid protocol length')
        if sum(body) & 255 != 0xaa and body[-1] != 0x88:
            raise ValueError('Invalid protocol checksum')
        packets.append((body[0], body[3:-1]))
    return packets


def integer(value):
    return int(value, 16 if value.lower().startswith('0x') else 10)


def import_rows(rows, bus, address):
    """Consume tshark fields up to the first TLS request; retain exact USB bytes."""
    steps, pending = [], bytearray()
    reached_tls = False
    for row in rows:
        if len(row) != len(FIELDS):
            raise ValueError('Expected seven tab-separated tshark fields')
        if not row[0] or not row[1] or (integer(row[0]), integer(row[1])) != (bus, address):
            continue
        endpoint = integer(row[2])
        if endpoint not in (0x01, 0x82):
            continue
        event = row[3].strip("'")
        event = ord(event) if event in ('S', 'C', 'E') else integer(event)
        status = integer(row[4])
        if event == ord('E') or (event == ord('C') and status != 0):
            raise ValueError('USB transfer failed during initialization')
        # OUT data is on submission, IN data on completion. Never count both.
        if (endpoint == 0x01 and event != ord('S')) or (endpoint == 0x82 and event != ord('C')):
            continue
        payload = bytes.fromhex(row[6].replace(':', ''))
        if len(payload) != integer(row[5]) or len(payload) > MAX_PACKET:
            raise ValueError('USB payload missing, truncated or oversized (record with snaplen 0)')
        if not payload:
            continue
        if endpoint == 0x82:
            if not steps or pending:
                raise ValueError('Unexpected response before a complete setup command')
            steps[-1][1].extend(payload)
            if len(steps[-1][1]) > MAX_PACKET:
                raise ValueError('Setup response is too large')
            continue
        pending.extend(payload)
        if len(pending) > MAX_PACKET:
            raise ValueError('Setup request is too large')
        if len(pending) < 4 or len(pending) < 4 + int.from_bytes(pending[1:3], 'little'):
            continue
        packets = protocol_packets(bytes(pending))
        if len(packets) != 1:
            raise ValueError('Expected one setup command per request')
        command = packets[0][0]
        if command == 0xd0:
            reached_tls = True
            break
        if len(steps) >= len(COMMANDS) or command != COMMANDS[len(steps)]:
            raise ValueError(f'Unexpected setup command 0x{command:02x} at exchange {len(steps)}')
        steps.append([bytes(pending), bytearray()])
        pending.clear()
    if not reached_tls or len(steps) != len(COMMANDS):
        raise ValueError('Capture must contain one complete initialization through the first TLS request')
    firmware = None
    for command, (_, response) in zip(COMMANDS, steps):
        packets = protocol_packets(bytes(response))
        if command == 0:
            if packets:
                raise ValueError('Unexpected NOP response')
            continue
        if (not packets or packets[0][0] != 0xb0 or len(packets[0][1]) != 2 or
                packets[0][1][0] != command or packets[0][1][1] not in (1, 3)):
            raise ValueError(f'Missing or unexpected ACK for 0x{command:02x}')
        needs_reply = command in (0xa8, 0xe4, 0xa2, 0x90)
        if len(packets) != (2 if needs_reply else 1):
            raise ValueError(f'Unexpected response count for 0x{command:02x}')
        if needs_reply and packets[1][0] != command:
            raise ValueError(f'Unexpected response command for 0x{command:02x}')
        if command == 0xa8:
            firmware = packets[1][1].rstrip(b'\0').decode('ascii')
    if firmware not in ('GF3206_RTSEC_APP_10052', 'GF3206_RTSEC_APP_10062'):
        raise ValueError(f'Unsupported recorded firmware: {firmware!r}')
    return steps, firmware


def write_fixture(file, steps, firmware, digest, revision, scenario, bus, address):
    config = configparser.ConfigParser(interpolation=None)
    config['replay'] = {
        'format': '1', 'provenance': 'imported usbmon setup; source authenticity not verified',
        'expected-usb-id': '27c6:55a2', 'bus': str(bus), 'address': str(address),
        'firmware': firmware, 'source-sha256': digest, 'source-revision': revision,
        'scenario': scenario, 'exchanges': str(len(steps)),
    }
    for i, (request, reply) in enumerate(steps):
        config[f'exchange-{i}'] = {'request': request.hex(), 'reply': reply.hex()}
    file.write('# Contains setup traffic only. The replay uses fresh test TLS and synthetic images.\n')
    config.write(file, space_around_delimiters=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path, help='New .ini file; existing files are never overwritten')
    parser.add_argument('--bus', required=True, type=int)
    parser.add_argument('--address', required=True, type=int)
    parser.add_argument('--source-revision', required=True, help='Git revision used for the recording')
    parser.add_argument('--scenario', choices=('steady', 'cold-boot', 'warm-boot', 'resume'), default='steady')
    parser.add_argument('--tsv', action='store_true', help='Input already contains the seven documented tshark fields')
    args = parser.parse_args()
    if not 1 <= args.bus <= 65535 or not 1 <= args.address <= 127:
        parser.error('Invalid USB bus/address')
    if not re.fullmatch(r'[0-9a-fA-F]{7,40}', args.source_revision):
        parser.error('Source revision must be a 7–40 digit hexadecimal Git revision')
    try:
        if args.capture.stat().st_size > 64 * 1024 * 1024:
            raise ValueError('Use a device-filtered setup capture smaller than 64 MiB')
        digest = hashlib.sha256(args.capture.read_bytes()).hexdigest()
        with tempfile.TemporaryFile(mode='w+', encoding='utf-8') as fields:
            if args.tsv:
                fields.write(args.capture.read_text())
            else:
                command = ['tshark', '-n', '-r', str(args.capture), '-Y',
                           f'usb.bus_id == {args.bus} && usb.device_address == {args.address}',
                           '-T', 'fields', '-E', 'occurrence=f']
                for field in FIELDS:
                    command.extend(('-e', field))
                subprocess.run(command, stdout=fields, check=True, timeout=60)
            fields.seek(0)
            steps, firmware = import_rows(csv.reader(fields, delimiter='\t'), args.bus, args.address)
        with args.output.open('x') as output:
            write_fixture(output, steps, firmware, digest, args.source_revision,
                          args.scenario, args.bus, args.address)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
