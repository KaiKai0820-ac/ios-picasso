#!/usr/bin/env python3
"""Repack a verified GUI-producing picasso boot with a quiet console.

Only the two embedded payload command lines and the Android image ID change.
The source image is preserved; this tool neither connects to nor flashes a phone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def align(size, page):
    return (size + page - 1) // page * page


def sections(blob):
    if blob[:8] != b'ANDROID!':
        raise ValueError('Expected an Android boot image')
    ks, ka, rs, ra, ss, sa, tags, page, version, osver = struct.unpack_from('<10I', blob, 8)
    dtbo_size, dtbo_offset, header_size, dtb_size, dtb_addr = struct.unpack_from('<IQIIQ', blob, 1632)
    if (version, page, header_size, ka, ra) != (2, 4096, 1660, 0x8000, 0x1200000):
        raise ValueError('Expected the known picasso v2 image layout')
    if ss or dtbo_size or dtbo_offset or sa:
        raise ValueError('Second-stage and recovery-DTBO payloads are unsupported')
    if dtb_addr != 0x1728000 or ks <= 0x40000 + rs or not dtb_size:
        raise ValueError('Missing picasso payload or full DTB')
    ramdisk = page + align(ks, page)
    dtb = ramdisk + align(rs, page)
    if len(blob) != dtb + align(dtb_size, page):
        raise ValueError('Unexpected trailing data or truncated image')
    return {
        'kernel': (page, ks), 'ramdisk': (ramdisk, rs),
        'second': (dtb, 0), 'recovery_dtbo': (dtb, 0), 'dtb': (dtb, dtb_size),
    }, osver


def image_id(blob, parts):
    digest = hashlib.sha1()
    for offset, size in parts.values():
        digest.update(blob[offset:offset + size])
        digest.update(struct.pack('<I', size))
    return digest.digest().ljust(32, b'\0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--marker', default='local31d')
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix('.json').exists():
        raise ValueError('Refusing to overwrite an existing result')
    original = args.source.read_bytes()
    original_sha = hashlib.sha256(original).hexdigest()
    if original_sha != args.source_sha256:
        raise ValueError('Source SHA256 mismatch')
    parts, osver = sections(original)
    if original[576:608] != image_id(original, parts):
        raise ValueError('Source Android image ID mismatch')
    payload_offset = parts['kernel'][0] + 0x40000
    ramdisk_offset, payload_size = parts['ramdisk']
    payload = original[payload_offset:payload_offset + payload_size]
    if payload != original[ramdisk_offset:ramdisk_offset + payload_size]:
        raise ValueError('Kernel and ramdisk payload copies differ')
    if payload[:8] != b'SWEETXNU' or struct.unpack_from('<I', payload, 8)[0] != 1:
        raise ValueError('Missing SWEETXNU v1 payload')
    if struct.unpack_from('<2Q', payload, 0x38) != (0xc0000000, 0x30000000):
        raise ValueError('Expected the hardware-proven contiguous 768 MiB RAM range')
    old_command = payload[0x48:0x48 + 608].split(b'\0', 1)[0].decode('ascii')
    words = old_command.split()
    keys = [word.split('=', 1)[0] for word in words]
    if len(keys) != len(set(keys)):
        raise ValueError('Ambiguous duplicate boot arguments')
    for required in ('rd=ufsroot0', 'ufsroot=userdata', 'sweettouch=0', 'sweetusb=2',
                     'sweetlogflush=1', 'sweetscan=-'):
        if required not in words:
            raise ValueError('Missing baseline argument: ' + required)
    if not args.marker or not args.marker.isascii() or not all(c.isalnum() or c in '_-' for c in args.marker):
        raise ValueError('Invalid boot marker')
    replacements = {'sweetfb_noconsole': '1', 'sweetufshb': '0', 'sweetboot': args.marker}
    words = [word for word in words if word.split('=', 1)[0] not in replacements]
    words.extend(key + '=' + value for key, value in replacements.items())
    command = ' '.join(words)
    encoded = command.encode('ascii')
    if len(encoded) >= 608:
        raise ValueError('Boot arguments would be truncated')
    output = bytearray(original)
    changes = []
    for offset in (payload_offset + 0x48, ramdisk_offset + 0x48):
        changes.append((offset, original[offset:offset + 608]))
        output[offset:offset + 608] = encoded.ljust(608, b'\0')
    output[576:608] = image_id(output, parts)
    restored = bytearray(output)
    restored[576:608] = original[576:608]
    for offset, before in changes:
        restored[offset:offset + 608] = before
    if restored != original or output == original:
        raise ValueError('Unexpected changes outside command lines and Android ID')
    if sections(output) != (parts, osver):
        raise ValueError('Boot header or layout changed')
    manifest = {
        'source_sha256': original_sha, 'sha256': hashlib.sha256(output).hexdigest(),
        'bytes': len(output), 'old_command': old_command, 'command': command,
        'os_version_word': hex(osver), 'ram_base': '0xc0000000', 'ram_size': '0x30000000',
        'dtb_sha256': hashlib.sha256(original[parts['dtb'][0]:][:parts['dtb'][1]]).hexdigest(),
        'only_changes': ['both payload command lines', 'Android image ID'],
        'phone_writes': False,
    }
    args.output.write_bytes(output)
    if hashlib.sha256(args.output.read_bytes()).hexdigest() != manifest['sha256']:
        raise IOError('Output readback mismatch')
    args.output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
