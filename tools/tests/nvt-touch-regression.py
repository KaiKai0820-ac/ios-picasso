#!/usr/bin/env python3
"""Exercise actual patched SPI/frame/HID code on the host, without a phone."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'static (?:int|void|bool|uint32_t)\n' + name + r'\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Already patched bsd/dev/picasso_nvt.c')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nvt-regression-') as name:
        root = Path(name)
        if args.source:
            source_path = args.source
        else:
            for number in ('0210', '0212', '0213', '0215'):
                patch, = (REPO / 'xnu/patches').glob(number + '-*.patch')
                # 0210 also changes build wiring; extract only the independent driver.
                subprocess.run(['git', 'apply', '--unsafe-paths', '--include=bsd/dev/picasso_nvt*',
                                str(patch)], cwd=root, check=True)
            source_path = root / 'bsd/dev/picasso_nvt.c'
        source = source_path.read_text()
        (root / 'picasso_nvt_protocol.h').write_bytes(source_path.with_name('picasso_nvt_protocol.h').read_bytes())
        defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define NV_'))
        prelude = '''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "picasso_nvt_protocol.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define NV_FW_VERSION 7u
typedef uintptr_t vm_offset_t;
static struct { vm_offset_t se, gpio_east; unsigned tx_depth, rx_depth; bool dead; } nv;
'''
        transfer = prelude + defines + '\n' + (REPO / 'tools/tests/nvt-spi-fixture.c').read_text()
        transfer += function(source, 'nv_transfer')
        transfer += '\nint main(void) { test_transfer(); return 0; }\n'
        pipeline = prelude + (REPO / 'tools/tests/nvt-events-fixture.c').read_text()
        pipeline += function(source, 'nv_release_contacts') + function(source, 'nv_run_touch')
        pipeline += '\nint main(void) { test_events(); return 0; }\n'
        for label, code in (('spi', transfer), ('events', pipeline)):
            path = root / (label + '.c')
            path.write_text(code)
            exe = root / label
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-g', str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        if source_path.with_name('picasso_nvt_firmware.h').exists():
            (root / 'picasso_nvt_firmware.h').write_bytes(source_path.with_name('picasso_nvt_firmware.h').read_bytes())
            firmware = prelude.replace('#define NV_FW_VERSION 7u\n', '')
            firmware += '#include "picasso_nvt_firmware.h"\n'
            firmware += (REPO / 'tools/tests/nvt-firmware-fixture.c').read_text()
            for name in ('nv_page', 'nv_register', 'nv_write_register', 'nv_write_byte',
                         'nv_read_memory', 'nv_bootloader_reset', 'nv_le32', 'nv_write_le',
                         'nv_wait_reset', 'nv_verify_firmware', 'nv_download_firmware', 'nv_initialize_panel'):
                firmware += function(source, name)
            firmware += '\nint main(void) { test_firmware(); return 0; }\n'
            path = root / 'firmware.c'
            path.write_text(firmware)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-fsanitize=address,undefined', '-g', str(path), '-o', str(root / 'firmware')], check=True)
            subprocess.run([str(root / 'firmware')], check=True)
        print('PASS: actual driver SPI FIFO streaming and contact/HID regressions (ASan/UBSan)')


if __name__ == '__main__':
    main()
