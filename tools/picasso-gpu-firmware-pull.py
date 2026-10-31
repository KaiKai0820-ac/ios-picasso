#!/usr/bin/env python3
"""Read the K30's four GPU firmware files from an already mounted Recovery.

No mount, reboot, hardware probe or device writes. Output belongs outside Git.
If Recovery has not mounted vendor/firmware, report that instead of guessing.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shlex
import subprocess

from picasso_gpu_firmware import PROPERTIES, validate

NAMES = ('a650_gmu.bin', 'a650_sqe.fw', 'a620_zap.mdt', 'a620_zap.b02')
LIMITS = (4 * 1024 * 1024, 0x40000, 1024 * 1024, 0x2000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    if output.is_relative_to(repo):
        parser.error('Choose an output directory outside the repository')

    def run(*words):
        return subprocess.check_output([args.adb, '-d', *words], timeout=120)

    def shell(command):
        return run('shell', command).decode().strip()

    if run('get-state').strip() != b'recovery' or shell('getprop ro.product.device') != 'picasso':
        raise ValueError('Expected the authorized K30 in Recovery')
    if 'uid=0(root)' not in shell('id'):
        raise ValueError('Recovery root ADB required')
    command = "find /vendor /firmware /vendor/firmware_mnt -type f \\( " + ' -o '.join(
        '-name ' + shlex.quote(name) for name in NAMES) + ' \\) 2>/dev/null'
    found = subprocess.run([args.adb, '-d', 'shell', command], capture_output=True,
        timeout=120, text=True)
    paths = {name: [] for name in NAMES}
    for line in found.stdout.splitlines():
        path = PurePosixPath(line.strip())
        if path.is_absolute() and '..' not in path.parts and path.name in paths:
            paths[path.name].append(str(path))
    missing = [name for name in NAMES if not paths[name]]
    if missing:
        raise RuntimeError('Not found in mounted firmware directories: ' + ', '.join(missing))
    blobs, records = {}, {}
    for name, key, limit in zip(NAMES, PROPERTIES, LIMITS):
        candidate = None
        sources = []
        for path in sorted(set(paths[name])):
            quoted = shlex.quote(path)
            size = int(shell('stat -c %s ' + quoted))
            if not 0 < size <= limit:
                raise ValueError('Unexpected firmware size: ' + name)
            data = run('exec-out', 'cat ' + quoted)
            digest = hashlib.sha256(data).hexdigest()
            if len(data) != size or shell('sha256sum ' + quoted).split()[0] != digest:
                raise ValueError('Independent device readback mismatch: ' + name)
            if candidate is not None and candidate != data:
                raise ValueError('Conflicting firmware copies: ' + name)
            candidate = data
            sources.append(path)
        blobs[key] = candidate
        records[name] = dict(sources=sources, bytes=len(candidate), sha256=hashlib.sha256(candidate).hexdigest())
    validation_error = None
    try:
        validation = validate(blobs)
    except ValueError as exc:
        validation, validation_error = None, str(exc)
    report = dict(validation=validation, validation_error=validation_error,
                  files=records, phone_writes=False)
    output.mkdir(parents=True, exist_ok=True)
    for name, key in zip(NAMES, PROPERTIES):
        path = output / name
        if path.exists() and path.read_bytes() != blobs[key]:
            raise ValueError('Refusing to replace different local firmware: ' + name)
    for name, key in zip(NAMES, PROPERTIES):
        path = output / name
        path.write_bytes(blobs[key])
        if path.read_bytes() != blobs[key]: raise IOError('Local readback mismatch: ' + name)
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if validation_error:
        raise SystemExit('Firmware preserved for diagnosis; packaging validation failed: ' + validation_error)


if __name__ == '__main__':
    main()
