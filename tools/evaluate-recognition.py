#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Evaluate NBIS on labeled assembled images; never changes authentication thresholds."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def load_samples(manifests):
    rows, paths, hashes = [], set(), set()
    required = ('path', 'finger_id', 'session', 'condition')
    for manifest in manifests:
        with manifest.open(newline='') as file:
            for row in csv.DictReader(file):
                if not all(isinstance(row.get(k), str) and row[k].strip() for k in required):
                    raise ValueError('Every row must contain path, finger_id, session and condition')
                path = (manifest.parent / row['path']).resolve()
                if path in paths:
                    raise ValueError('Duplicate capture paths would bias the evaluation')
                if path.stat().st_size > 17 * 1024 * 1024:
                    raise ValueError(f'Capture is too large: {path}')
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                if digest in hashes:
                    raise ValueError('Identical captures under different paths would bias the evaluation')
                rows.append({**{k: row[k].strip() for k in required}, 'path': str(path), 'sha256': digest})
                paths.add(path)
                hashes.add(digest)
                if len(rows) > 256:
                    raise ValueError('Provide between 2 and 256 captures')
    if len(rows) < 2:
        raise ValueError('Provide between 2 and 256 captures')
    return rows


def summarize(samples, result, threshold):
    if threshold < 1:
        raise ValueError('Threshold must be positive')
    images = result['images']
    usable = {x['index']: x['usable'] for x in images}
    if (len(images) != len(samples) or set(usable) != set(range(len(samples))) or
            any(type(value) is not bool for value in usable.values())):
        raise ValueError('Scorer returned inconsistent image metadata')
    expected = {(i, j) for i in usable for j in usable if i != j and usable[i] and usable[j]}
    seen = set()
    genuine, impostor = [], []
    by_condition = {}
    for row in result['scores']:
        pair = (row['probe'], row['gallery'])
        if (pair not in expected or pair in seen or type(row['score']) is not int or row['score'] < 0):
            raise ValueError('Scorer returned duplicate or invalid comparisons')
        seen.add(pair)
        left, right = samples[row['probe']], samples[row['gallery']]
        same = left['finger_id'] == right['finger_id']
        if same and left['session'] == right['session']:
            continue  # Avoid optimistic comparisons within an enrollment session.
        score = row['score']
        (genuine if same else impostor).append(score)
        label = left['condition']
        group = by_condition.setdefault(label, {'genuine_trials': 0, 'false_rejections': 0,
                                               'impostor_trials': 0, 'false_acceptances': 0})
        group['genuine_trials' if same else 'impostor_trials'] += 1
        group['false_rejections' if same else 'false_acceptances'] += int(score < threshold if same else score >= threshold)
    if seen != expected:
        raise ValueError('Scorer did not return every usable comparison')
    def stats(scores, reject):
        errors = sum(x < threshold if reject else x >= threshold for x in scores)
        return {'trials': len(scores), 'errors': errors,
                'error_rate': errors / len(scores) if scores else None,
                'minimum': min(scores) if scores else None,
                'median': statistics.median(scores) if scores else None,
                'maximum': max(scores) if scores else None}
    return {'threshold': threshold, 'images': len(samples),
            'feature_extraction_failures': sum(not usable[i] for i in range(len(samples))),
            'genuine': stats(genuine, True), 'impostor': stats(impostor, False),
            'by_probe_condition': by_condition,
            'interpretation': 'Single-template, ordered comparisons. Same-finger same-session pairs excluded. '
                              'Rates exclude feature extraction failures. Pairs share images and are not independent trials. '
                              'This is not a production false-acceptance guarantee.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path, nargs='+', help='One or more CSVs: path,finger_id,session,condition')
    parser.add_argument('--scorer', type=Path,
                        default=Path('/opt/libfprint-goodix/libexec/libfprint-2/goodix-score'),
                        help='Scoring executable (default: isolated Arch package helper)')
    parser.add_argument('--threshold', type=int, default=40, help="Match threshold (default: the driver's, 40)")
    args = parser.parse_args()
    if args.threshold < 1:
        parser.error('Threshold must be positive')
    try:
        rows = load_samples(args.manifest)
        result = subprocess.run([str(args.scorer.resolve()), *(row['path'] for row in rows)],
                                capture_output=True, text=True, check=True, timeout=600)
        scores = json.loads(result.stdout)
        report = summarize(rows, scores, args.threshold)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
    report['measurements'] = scores
    report['samples'] = rows
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
