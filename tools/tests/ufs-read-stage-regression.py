#!/usr/bin/env python3
"""Exercise the actual strategy and copy functions with deterministic interleavings.

Usage: python3 tools/tests/ufs-read-stage-regression.py BEFORE.c AFTER.c
The original must exhibit corruption; the patched source must preserve bytes.
No kernel, device or downloaded artifacts are required beyond the two sources.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, name):
    start = source.index('static void\n' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    harness = Path(__file__).with_suffix('.c').resolve()
    with tempfile.TemporaryDirectory(prefix='ufs-stage-test-') as directory:
        directory = Path(directory)
        for fixed, path in enumerate((args.before, args.after)):
            source = path.read_text()
            copy = function(source, 'ufs_copy_out').replace(
                'ufs_copy_out(', 'ufs_copy_out_impl(', 1)
            strategy = function(source, 'sweet_ufs_strategy')
            (directory / 'strategy.h').write_text(copy + '\n' + strategy + '\n')
            binary = directory / ('fixed' if fixed else 'original')
            subprocess.run(['cc', '-std=gnu11', '-g', '-O1', '-pthread',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-DEXPECT_PRIVATE=' + str(fixed), '-I', str(directory),
                str(harness), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    main()
