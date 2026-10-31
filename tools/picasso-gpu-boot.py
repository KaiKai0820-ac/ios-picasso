#!/usr/bin/env python3
"""Build and independently verify an offline GPU test boot from local31j.

Run under Linux/WSL with aarch64-linux-gnu tools and mkbootimg installed.
No phone connection, partition writes, firmware downloads, or baseline changes.
Omit --kernel for a complete baseline/arguments/ADT preflight.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

import adt
import mkadt
import mkpayload
from picasso_gpu_firmware import PROPERTIES, validate

REPO = Path(__file__).resolve().parents[1]
BASELINE_SHA = 'd00e97e3e59b8fba8eed8d566d2fd091af5d634246dc1c06fd4ad30c5c39b808'
DTB_SHA = '82c9a952aa9155a95e4a7f781e76941fc313e0fe409d1514f15a21a7101b10e3'
spec = importlib.util.spec_from_file_location('quiet', REPO / 'tools/picasso-quiet-boot.py')
quiet = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quiet)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def nodes(root):
    yield root
    for child in root.children:
        yield from nodes(child)


def prepare(original, marker, level, delay, zap, keep, present, firmware):
    if sha(original) != BASELINE_SHA:
        raise ValueError('Expected verified user-confirmed local31j baseline')
    if not re.fullmatch(r'gpu[0-9a-z_-]{1,20}', marker):
        raise ValueError('Use a unique GPU boot marker, for example gpu31a')
    if not 0 <= level <= 22 or not 0 <= delay <= 600 or not 0 <= zap <= 3:
        raise ValueError('Invalid stage, delay or ZAP mode')
    if level >= 11 and firmware is None:
        raise ValueError('Stage 11+ requires all four validated device firmware blobs')
    if keep and (level < 21 or zap != 3):
        raise ValueError('Kept service requires stage 21+ and full authenticated ZAP mode 3')
    if level < 20 and zap:
        raise ValueError('ZAP mode is relevant only at stage 20+')
    parts, osword = quiet.sections(original)
    if osword != 0x1e0001a6 or original[576:608] != quiet.image_id(original, parts):
        raise ValueError('Baseline header/Android image ID mismatch')
    offset, size = parts['ramdisk']
    payload = original[offset:offset + size]
    if payload[:8] != b'SWEETXNU' or payload != original[4096 + 0x40000:4096 + 0x40000 + size]:
        raise ValueError('Baseline payload copies differ')
    if struct.unpack_from('<2Q', payload, 0x38) != (0xc0000000, 0x30000000):
        raise ValueError('Baseline RAM layout differs')
    ao, az = struct.unpack_from('<2Q', payload, 0x20)
    old_adt = payload[ao:ao + az]
    root, end = adt.parse(old_adt)
    if end != len(old_adt) or adt.serialize(root) != old_adt:
        raise ValueError('ADT did not round-trip exactly')
    if any(n.name == 'sweet-gpu' for n in root.children):
        raise ValueError('Baseline already contains a GPU node')
    gpu = next(n for n in mkadt.build(board='picasso', seed=bytes(64),
        gpu_probe=True, gpu_firmware=firmware).children if n.name == 'sweet-gpu')
    handles = [int.from_bytes(n.prop('AAPL,phandle'), 'little')
        for n in nodes(root) if n.prop('AAPL,phandle')]
    gpu.props = [(name, flags, struct.pack('<I', max(handles, default=0) + 1)
        if name == 'AAPL,phandle' else value) for name, flags, value in gpu.props]
    root.children.append(gpu)
    new_adt = adt.serialize(root)
    root.children.pop()
    if adt.serialize(root) != old_adt or mkpayload.adt_dram(new_adt) != (0xc0000000, 0x30000000):
        raise ValueError('Unexpected baseline ADT change')
    dtb_at, dtb_size = parts['dtb']
    dtb = original[dtb_at:dtb_at + dtb_size]
    if len(dtb) != 1663798 or sha(dtb) != DTB_SHA:
        raise ValueError('Full stock 12-tree DTB bundle differs')
    command = payload[0x48:0x48 + 608].split(b'\0', 1)[0].decode('ascii')
    words = command.split()
    for required in ('sweetboot=local31j', 'picassonvt=1', 'picassonvtprobe=0',
                     'rd=ufsroot0', 'ufsroot=userdata', 'sweettouch=0', 'sweetusb=2',
                     'sweetlogflush=1', 'sweetscan=-', 'sweetfb_noconsole=1', 'sweetufshb=0'):
        if required not in words:
            raise ValueError('Missing baseline argument ' + required)
    replacements = dict(sweetboot=marker, picassogpu=str(level), picassogpudelay=str(delay))
    if zap: replacements['picassogpuzap'] = str(zap)
    if keep: replacements['picassogpukeep'] = '1'
    if present: replacements.update(picassopresent='1', picassofbstats='1')
    words = [word for word in words if word.split('=', 1)[0] not in replacements]
    words += [key + '=' + value for key, value in replacements.items()]
    if len({word.split('=', 1)[0] for word in words}) != len(words):
        raise ValueError('Duplicate boot arguments')
    command = ' '.join(words)
    if len(command.encode('ascii')) >= 608:
        raise ValueError('GPU command line would be truncated')
    return parts, osword, new_adt, dtb, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=REPO / 'shim/out/shim-boot-local31j.img')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--kernel-sha256')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--marker', default='gpu31a')
    parser.add_argument('--level', type=int, default=3)
    parser.add_argument('--delay', type=int, default=180)
    parser.add_argument('--zap', type=int, default=0)
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--present', action='store_true')
    for name in ('gmu-fw', 'sqe-fw', 'zap-mdt', 'zap-seg'):
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    paths = (args.gmu_fw, args.sqe_fw, args.zap_mdt, args.zap_seg)
    if any(paths) and not all(paths): parser.error('Supply all four firmware paths together')
    firmware = {name: path.read_bytes() for name, path in zip(PROPERTIES, paths)} if all(paths) else None
    firmware_info = validate(firmware) if firmware is not None else None
    original = args.baseline.read_bytes()
    parts, osword, tree, dtb, command = prepare(original, args.marker, args.level,
        args.delay, args.zap, args.keep, args.present, firmware)
    report = dict(marker=args.marker, gpu_level=args.level, gpu_delay=args.delay,
        gpu_zap=args.zap, gpu_keep=args.keep, presenter=args.present, touch_enabled=True,
        source_sha256=sha(original), adt_sha256=sha(tree), dtb_sha256=sha(dtb), command=command,
        firmware=firmware_info, os_version_word=hex(osword), ram_base='0xc0000000',
        ram_size='0x30000000', phone_writes=False)
    if not args.kernel:
        print(json.dumps(dict(report, preflight=True), indent=2))
        return
    if not args.output or not args.kernel_sha256: parser.error('--output and --kernel-sha256 required')
    if args.output.exists() or args.output.with_suffix('.json').exists():
        raise ValueError('Refusing to replace an existing named result')
    kernel = args.kernel.read_bytes()
    if sha(kernel) != args.kernel_sha256:
        raise ValueError('Kernel SHA256 mismatch')
    for required in (b'PICASSO GPU:', b'PICASSO NVT: HID touch enabled;'):
        if required not in kernel: raise ValueError('Missing GPU or baseline touch driver')
    with tempfile.TemporaryDirectory(prefix='picasso-gpu-boot-') as directory:
        work = Path(directory)
        for source in (REPO / 'shim').iterdir():
            if source.is_file(): shutil.copy2(source, work / source.name)
        treefile, dtbfile = work / 'picasso.adt', work / 'full-stock.dtb'
        treefile.write_bytes(tree)
        dtbfile.write_bytes(dtb)
        payload, kernel_info = mkpayload.build(args.kernel.resolve(), treefile, command)
        payloadfile = work / 'payload.bin'
        payloadfile.write_bytes(payload)
        subprocess.run(['bash', str(work / 'build.sh'), str(payloadfile), '--picasso',
            '--dtb', str(dtbfile), '--ram-base', '0xc0000000', '--ram-size', '0x30000000',
            '--os-version', '15.0.0', '--os-patch-level', '2026-06'], cwd=work, check=True)
        image = (work / 'out/shim-boot.img').read_bytes()
        newparts, newos = quiet.sections(image)
        if newos != osword or image[576:608] != quiet.image_id(image, newparts):
            raise ValueError('Result header/Android image ID mismatch')
        ro, rs = newparts['ramdisk']
        if (image[ro:ro + rs] != payload or
            image[4096 + 0x40000:4096 + 0x40000 + rs] != payload or
            image[newparts['dtb'][0]:][:newparts['dtb'][1]] != dtb or
            image[4096 + 0x40000 + quiet.align(len(payload), 4096):][:len(dtb)] != dtb or
            payload[0x48:0x48 + 608].split(b'\0', 1)[0].decode() != command or
            struct.unpack_from('<2Q', payload, 0x38) != (0xc0000000, 0x30000000)):
            raise ValueError('Result payload/DTB/command/RAM verification failed')
        ks, ka, rs, ra, ss, sa, tags, page, version, _ = struct.unpack_from('<10I', image, 8)
        _, dtb_address = struct.unpack_from('<IQ', image, 1648)
        bootcommand = (image[64:576] + image[608:1632]).split(b'\0', 1)[0].decode()
        if bootcommand != (original[64:576] + original[608:1632]).split(b'\0', 1)[0].decode():
            raise ValueError('Shim command line changed')
        for name in ('kernel', 'ramdisk', 'dtb'):
            at, size = newparts[name]
            (work / name).write_bytes(image[at:at + size])
        independent = work / 'independent.img'
        subprocess.run(['mkbootimg', '--header_version', str(version), '--base', '0',
            '--kernel', str(work / 'kernel'), '--ramdisk', str(work / 'ramdisk'), '--dtb', str(work / 'dtb'),
            '--kernel_offset', hex(ka), '--ramdisk_offset', hex(ra), '--tags_offset', hex(tags),
            '--dtb_offset', hex(dtb_address), '--pagesize', str(page), '--cmdline', bootcommand,
            '--os_version', '15.0.0', '--os_patch_level', '2026-06', '-o', str(independent)], check=True)
        if independent.read_bytes() != image: raise ValueError('Independent mkbootimg differs')
    if args.baseline.read_bytes() != original: raise ValueError('Baseline changed')
    report.update(sha256=sha(image), bytes=len(image), kernel_sha256=sha(kernel),
        payload_sha256=sha(payload), independent_mkbootimg='byte-identical', kernel_info=kernel_info)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    if args.output.read_bytes() != image: raise ValueError('Output readback mismatch')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
