#!/usr/bin/env python3
"""Generate the ordered XNU patch from the reviewed Picasso GPU sources."""
import argparse
import difflib
from pathlib import Path

FILES = {
    'picasso_gpu.c': 'bsd/dev/picasso_gpu.c',
    'picasso_gpu_protocol.h': 'bsd/dev/picasso_gpu_protocol.h',
    'picasso_gpu_tables.h': 'bsd/dev/picasso_gpu_tables.h',
    'SweetGPU.cpp': 'iokit/Kernel/arm/SweetGPU.cpp',
}


def build():
    sources = Path(__file__).resolve().parent.parent / 'xnu/picasso-gpu'
    parts = [(sources / 'integration.patch').read_text(encoding='utf-8')]
    for name, target in FILES.items():
        data = (sources / name).read_text(encoding='utf-8')
        if not data.endswith('\n'):
            raise ValueError(f'{name}: missing final newline')
        parts.append(''.join(difflib.unified_diff([], data.splitlines(True),
            fromfile='/dev/null', tofile='b/' + target)))
    return ''.join(parts)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(build(), encoding='utf-8', newline='\n')
