#!/usr/bin/env python3
"""Actual-source GPU allocator/ring checks and firmware/ADT compatibility checks."""
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import adt
import mkadt
from picasso_gpu_firmware import validate


def function(source, name):
    start = re.search(r'(?:static )?(?:bool|uint32_t|uint64_t|void)\n' + name + r'\(', source).start()
    opened = source.index('{', start)
    level, end = 1, opened + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def blobs():
    mdt = bytearray(180)
    mdt[:6] = b'\x7fELF\x01\x01'
    struct.pack_into('<I', mdt, 28, 52)
    struct.pack_into('<HH', mdt, 42, 32, 3)
    struct.pack_into('<8I', mdt, 84, 1, 0, 0, 0, 32, 32, 2 << 24, 0)
    struct.pack_into('<8I', mdt, 116, 1, 0, 0, 0, 128, 256, 0, 0)
    return {'gmu-firmware': struct.pack('<4I', 0, 16, 0, 0) + bytes(16),
            'sqe-firmware': struct.pack('<2I', 0, 0x95),
            'zap-metadata': bytes(mdt), 'zap-segment': bytes(128)}


def main():
    source = (REPO / 'xnu/picasso-gpu/picasso_gpu.c').read_text()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        code = '''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picasso_gpu_protocol.h"
typedef uintptr_t vm_offset_t;
'''
        code += '\n'.join(line for line in source.splitlines() if line.startswith('#define ')) + '\n'
        code += '''static struct { bool on; vm_offset_t gpu, smmu, ring_mem, scratch;
uint32_t pg,npg,at,sent,fence,khz; } kept;
static struct { uint64_t start,size; } kept_free[KEPT_FREE_RUNS];
static unsigned kept_free_count,rptr,wptr,waits;
static uint32_t gpu_rd(vm_offset_t b,uint32_t o) {(void)b;assert(o==GPU_CP_RB_RPTR);return rptr;}
static void gpu_wr(vm_offset_t b,uint32_t o,uint32_t v) {(void)b;assert(o==GPU_CP_RB_WPTR);wptr=v;}
static void gpu_to_memory(vm_offset_t p,uint64_t n) {(void)p;(void)n;}
static void delay(unsigned n) {(void)n;++waits;}
'''
        for name in ('gpu_parity', 'kept_span', 'sweet_gpu_address', 'sweet_gpu_address_free',
                     'kept_ring', 'kept_pkt7', 'sweet_gpu_submit', 'sweet_gpu_done'):
            code += function(source, name)
        client = (REPO / 'xnu/picasso-gpu/SweetGPU.cpp').read_text()
        code += '''
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define GPU_PATIENCE_MS 2000
struct msm_timespec { int64_t seconds, nanoseconds; };
static uint64_t mach_continuous_time(void) { return 10000000000ULL; }
static void absolutetime_to_nanoseconds(uint64_t at, uint64_t *ns) { *ns = at; }
'''
        code += function(client, 'millisecondsUntil')
        code += (REPO / 'tools/tests/picasso-gpu-fixture.c').read_text()
        (root / 'regression.c').write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-I' + str(REPO / 'xnu/picasso-gpu'),
                        str(root / 'regression.c'), '-o', str(root / 'regression')], check=True)
        subprocess.run([str(root / 'regression')], check=True)
        original = subprocess.check_output(['git', 'show', '03fe3ab:tools/mkadt.py'], cwd=REPO)
        (root / 'old_mkadt.py').write_bytes(original)
        spec = importlib.util.spec_from_file_location('old_mkadt', root / 'old_mkadt.py')
        old = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(old)
        old.d22ap_product.__defaults__ = mkadt.d22ap_product.__defaults__
        for options in ({'board': 'sweet'}, {'board': 'picasso'}, {'qemu': True},
                        {'board': 'picasso', 'ram_base': 0x140000000, 'ram_size': 0x40000000}):
            assert adt.serialize(old.build(seed=bytes(64), **options)) == adt.serialize(mkadt.build(seed=bytes(64), **options))
        good = blobs()
        validate(good)
        mkadt.build(seed=bytes(64), board='picasso', gpu_firmware=good)
        for key in good:
            bad = dict(good); bad[key] = bad[key][:3]
            try: validate(bad)
            except ValueError: pass
            else: raise AssertionError(key)
        for options in ({'gpu_probe': True}, {'gpu_probe': True, 'qemu': True, 'board': 'picasso'}):
            try: mkadt.build(**options)
            except ValueError: pass
            else: raise AssertionError(options)
        print('PASS unchanged default ADT, explicit board gate, full firmware packaging and malformed inputs')


if __name__ == '__main__':
    main()
