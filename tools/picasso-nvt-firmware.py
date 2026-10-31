#!/usr/bin/env python3
"""Build a header from the pinned, publicly released Picasso touch firmware.

Firmware bytes and generated headers stay out of Git. Both the public Intel HEX
source and decoded binary must match the previously verified Recovery firmware.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import struct
import urllib.request

REVISION = '8e49d06af7ff5757c03ab181874cf1e90a1e1192'
URL = ('https://raw.githubusercontent.com/LineageOS/android_kernel_xiaomi_sm7250/'
       + REVISION + '/firmware/novatek_nt36672c_g7a_fw02.bin.ihex')
SOURCE_SHA = '7d2109657271cae88d0986fc7ead0f4a6ff920a13ac057e391437b434ce8efae'
BINARY_SHA = '9daa042550e1c8bd703c18b4ad6d0218a46bc24e6a4a7667417c1c4edcf495b3'


def decode_ihex(text):
    memory = {}
    upper = 0
    eof = False
    for line in text.splitlines():
        if not line.strip():
            continue
        if eof or not line.startswith(':'):
            raise ValueError('Unexpected Intel HEX record')
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 255:
            raise ValueError('Invalid Intel HEX length/checksum')
        size, kind = record[0], record[3]
        address = int.from_bytes(record[1:3], 'big')
        payload = record[4:-1]
        if kind == 0:
            start = upper + address
            if start + size > 0x30000:
                raise ValueError('Firmware address outside expected image')
            for offset, value in enumerate(payload):
                if start + offset in memory:
                    raise ValueError('Overlapping Intel HEX data')
                memory[start + offset] = value
        elif kind == 4 and size == 2 and address == 0:
            upper = int.from_bytes(payload, 'big') << 16
        elif kind == 2 and size == 2 and address == 0:
            upper = int.from_bytes(payload, 'big') << 4
        elif kind == 1 and size == 0 and address == 0:
            eof = True
        else:
            raise ValueError('Unsupported Intel HEX record')
    if not eof or not memory or set(memory) != set(range(max(memory) + 1)):
        raise ValueError('Incomplete Intel HEX image')
    return bytes(memory[index] for index in range(len(memory)))


def partitions(blob):
    if len(blob) < 0x30:
        raise ValueError('Truncated firmware header')
    word = lambda offset: struct.unpack_from('<I', blob, offset)[0]
    header_end = word(0)
    if header_end < 0x30 or header_end > len(blob) or (header_end - 0x30) % 16:
        raise ValueError('Invalid firmware header size')
    entries = [(word(i * 12), word(i * 12 + 4), word(i * 12 + 8), word(0x18 + i * 4))
               for i in range(2)]
    entries += [(word(o + 8), word(o), word(o + 4), word(o + 12))
                for o in range(0x30, header_end, 16)]
    if blob[0x28] & 0x10:
        for index in range(blob[0x28] & 15):
            offset = entries[1][0] + index * 16
            if offset + 16 > len(blob):
                raise ValueError('Truncated overlay header')
            entries.append((word(offset + 8), word(offset), word(offset + 4), word(offset + 12)))
    if not entries[0][2] or not entries[1][2]:
        raise ValueError('Missing ILM/DLM')
    active = []
    for image, sram, size, crc in entries:
        if not size:  # Vendor reserved entries contain no data.
            continue
        length = size + 1  # Vendor format stores the final byte index.
        if image + length > len(blob) or sram + length > 0x30000:
            raise ValueError('Firmware partition outside image/SRAM')
        active.append((image, sram, size, crc))
    return active


def header(blob):
    parts = partitions(blob)
    lines = ['/* Generated from pinned public Xiaomi/LineageOS firmware; do not commit. */',
             '#define NV_FW_VERSION 7u', '#define NV_FW_SHA256 "' + BINARY_SHA + '"',
             'struct nv_fw_partition { uint32_t image, sram, size, crc; };',
             'static const struct nv_fw_partition nv_fw_parts[] = {']
    lines += ['\t{' + ', '.join(hex(value) + 'u' for value in entry) + '},' for entry in parts]
    lines += ['};', 'static const uint8_t nv_firmware[] = {']
    lines += ['\t' + ','.join('0x%02x' % value for value in blob[offset:offset + 16]) + ','
              for offset in range(0, len(blob), 16)]
    return '\n'.join(lines + ['};', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ihex', type=Path, help='Use an already downloaded source')
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument('--header-output', type=Path)
    output.add_argument('--patch-output', type=Path)
    args = parser.parse_args()
    if args.ihex:
        source = args.ihex.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=40) as response:
            source = response.read()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA:
        raise ValueError('Public firmware source SHA256 mismatch')
    blob = decode_ihex(source.decode('ascii'))
    if len(blob) != 139264 or hashlib.sha256(blob).hexdigest() != BINARY_SHA:
        raise ValueError('Decoded firmware differs from verified Recovery image')
    if blob[131072 - 3:131072] != b'NVT' or blob[126976:126978] != bytes((7, 248)):
        raise ValueError('Unexpected firmware version or download end marker')
    generated = header(blob)
    target = args.header_output or args.patch_output
    if args.patch_output:
        generated = ''.join(difflib.unified_diff([], generated.splitlines(True),
            fromfile='/dev/null', tofile='b/bsd/dev/picasso_nvt_firmware.h'))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(generated, encoding='utf-8', newline='\n')
    print(json.dumps(dict(binary_sha256=BINARY_SHA, bytes=len(blob),
        partitions=partitions(blob), output=str(target))))


if __name__ == '__main__':
    main()
