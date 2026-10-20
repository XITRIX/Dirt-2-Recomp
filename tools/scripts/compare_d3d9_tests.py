#!/usr/bin/env python3
"""Compare identical D3D9 regression runs. Exit nonzero on errors/mismatches.

No dependencies. Saves a JSON comparison and PNGs for mismatching images.
Usage: compare_d3d9_tests.py REFERENCE_DIR CANDIDATE_DIR --out DIFF_DIR
"""
import argparse
import json
from pathlib import Path
import struct
import zlib


def png(path, width, height, rgba):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def load(directory):
    report = json.loads((directory / 'results.json').read_text())
    if report['schema'] != 1 or report['suite'] != 'dirt2-d3d9-v1':
        raise ValueError(f'{directory}: unsupported suite/schema')
    tests = {t['name']: t for t in report['tests']}
    if not tests or len(tests) != len(report['tests']):
        raise ValueError(f'{directory}: empty suite or duplicate names')
    return report, tests


def read_image(directory, test, expected_bytes):
    name = test['image']
    if not name or Path(name).name != name:
        raise ValueError(f'{test["name"]}: missing/invalid image filename')
    image = (directory / name).read_bytes()
    if len(image) != expected_bytes:
        raise ValueError(f'{name}: expected {expected_bytes} bytes, got {len(image)}')
    return image


def compare(reference, candidate, output, tolerance):
    if not 0 <= tolerance <= 255:
        raise ValueError('tolerance must be between 0 and 255')
    ref, ref_tests = load(reference)
    got, got_tests = load(candidate)
    dims = ref['width'], ref['height']
    if dims != (got['width'], got['height']) or not all(0 < n <= 4096 for n in dims):
        raise ValueError('invalid/mismatched image dimensions')
    output.mkdir(parents=True, exist_ok=True)
    failures = []
    if ref['error'] or ref['failures']:
        failures.append('reference run has failures; cannot certify a baseline')
    if got['error'] or got['failures']:
        failures.append('candidate run has expectation/API failures')
    comparisons = []
    for name in sorted(ref_tests.keys() | got_tests.keys()):
        item = {'name': name}
        try:
            a, b = ref_tests[name], got_tests[name]
            case_error = (f'reference: {a["error"]}; candidate: {b["error"]}'
                          if a['status'] != 'pass' or b['status'] != 'pass' else '')
            expected_bytes = dims[0] * dims[1] * 4
            left = read_image(reference, a, expected_bytes)
            right = read_image(candidate, b, expected_bytes)
            mismatch = 0
            maximum = 0
            diff = bytearray(expected_bytes)
            rgba = bytearray(expected_bytes)
            for i in range(0, expected_bytes, 4):
                delta = [abs(left[i+c] - right[i+c]) for c in range(4)]
                maximum = max(maximum, *delta)
                mismatch += max(delta) > tolerance
                diff[i:i+4] = bytes([min(255, delta[2]*8), min(255, delta[1]*8), min(255, delta[0]*8), 255])
                rgba[i:i+4] = bytes([right[i+2], right[i+1], right[i], right[i+3]])
            item.update(mismatch_pixels=mismatch, max_channel_delta=maximum)
            if mismatch:
                png(output / f'{name}-diff.png', *dims, diff)
                png(output / f'{name}-candidate.png', *dims, rgba)
                raise ValueError(f'{mismatch} pixels differ beyond tolerance {tolerance}; {case_error}')
            if case_error:
                raise ValueError(case_error)
            item['status'] = 'pass'
        except (KeyError, ValueError, OSError) as e:
            item.update(status='fail', error=str(e))
            failures.append(f'{name}: {e}')
        comparisons.append(item)
    report = {'reference': str(reference), 'candidate': str(candidate), 'tolerance': tolerance,
              'failures': failures, 'tests': comparisons}
    (output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(comparisons)} cases, {len(failures)} failures; {output / "comparison.json"}')
    for failure in failures:
        print(f'  {failure}')
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tolerance', type=int, default=2)
    args = parser.parse_args()
    try:
        return compare(args.reference, args.candidate, args.out, args.tolerance)
    except (KeyError, ValueError, OSError) as e:
        parser.exit(2, f'Invalid report: {e}\n')


if __name__ == '__main__':
    raise SystemExit(main())
