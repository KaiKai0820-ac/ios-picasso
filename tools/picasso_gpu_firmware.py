"""Validate externally supplied A620 firmware before embedding it in an ADT.

This module never downloads or writes firmware. Device-specific blobs stay out
of Git; callers record returned hashes alongside the generated boot artifact.
"""
import hashlib
import struct

PROPERTIES = ('gmu-firmware', 'sqe-firmware', 'zap-metadata', 'zap-segment')


def validate(blobs):
    if set(blobs) != set(PROPERTIES):
        raise ValueError('supply GMU, SQE, ZAP metadata and ZAP segment together')
    gmu, sqe, mdt, segment = (blobs[key] for key in PROPERTIES)
    if not 32 <= len(gmu) <= 4 * 1024 * 1024:
        raise ValueError('invalid GMU size')
    at, payloads = 0, 0
    while at < len(gmu):
        if len(gmu) - at < 16:
            raise ValueError('truncated GMU header')
        address, size, kind, value = struct.unpack_from('<4I', gmu, at)
        at += 16
        if size > len(gmu) - at or (size | address) & 3:
            raise ValueError('invalid GMU block size/alignment')
        if size:
            limit = 0x4000 if address < 0x4000 else (
                0x10008000 if 0x10004000 <= address < 0x10008000 else 0x80000000)
            if address >= limit or size > limit - address:
                raise ValueError('GMU block crosses its memory region')
            payloads += 1
        elif kind in (1, 7):
            if not 0 < value <= 0x1000000 or address & 4095 or address + value > 0x80000000:
                raise ValueError('invalid GMU preallocation')
        at += size
    if not payloads:
        raise ValueError('GMU firmware has no payload')
    if len(sqe) < 8 or len(sqe) > 0x40000 or len(sqe) & 3:
        raise ValueError('invalid SQE size')
    sqe_version = struct.unpack_from('<I', sqe, 4)[0] & 0xfff
    if sqe_version < 0x95:
        raise ValueError(f'SQE version {sqe_version:#x} is below 0x95')
    if len(mdt) < 116 or mdt[:6] != b'\x7fELF\x01\x01':
        raise ValueError('ZAP MDT must be a little-endian ELF32')
    phoff = struct.unpack_from('<I', mdt, 28)[0]
    phsize, count = struct.unpack_from('<HH', mdt, 42)
    if phoff != 52 or phsize != 32 or not 3 <= count <= 32 or 52 + count * 32 > len(mdt):
        raise ValueError('invalid ZAP program header table')
    if 52 + count * 32 + struct.unpack_from('<I', mdt, 100)[0] != len(mdt):
        raise ValueError('ZAP metadata hash length mismatch')
    loads = []
    for index in range(count):
        kind, _, _, _, filesz, memsz, flags, _ = struct.unpack_from('<8I', mdt, 52 + index * 32)
        if kind != 1 or (flags >> 24) & 7 == 2:
            continue
        if not 0 < filesz == len(segment) <= memsz <= 0x2000:
            raise ValueError('ZAP segment does not fit the K30 reservation or MDT')
        loads.append(index)
    if loads != [2]:
        raise ValueError('expected one loadable ZAP segment, .b02')
    return dict(chip_id='0x06020001', sqe_version=hex(sqe_version), gmu_payloads=payloads,
                firmware={key: dict(bytes=len(value), sha256=hashlib.sha256(value).hexdigest())
                          for key, value in blobs.items()})
