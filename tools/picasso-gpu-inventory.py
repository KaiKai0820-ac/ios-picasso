#!/usr/bin/env python3
"""Inventory A620 configuration from a saved DTB; no device connection or MMIO.

Usage: picasso-gpu-inventory.py DTB [--tree-index N] [--output REPORT.json]
For a concatenated boot DTB, select a tree explicitly. Firmware filenames and
addresses are configuration evidence, not proof of loaded firmware or running clocks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from picasso_fdt import FDT_MAGIC, parse, reg_entries


def strings(node, key):
    return [part.decode('utf-8', 'replace') for part in (node.get(key) or b'').split(b'\0') if part]


def cells(node, key):
    return list(node.cells(key) or ())


def regions(node):
    if node is None:
        return []
    names = strings(node, 'reg-names')
    return [dict(name=names[index] if index < len(names) else str(index),
                 base=hex(base), size=hex(size))
            for index, (base, size) in enumerate(reg_entries(node))]


def trees(data):
    result = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < 40:
            raise ValueError(f'truncated FDT header at {offset:#x}')
        magic, size = struct.unpack_from('>II', data, offset)
        if magic != FDT_MAGIC or size < 40 or size > len(data) - offset:
            raise ValueError(f'invalid FDT boundary at {offset:#x}')
        root, _ = parse(data[offset:offset + size])
        result.append((offset, size, root))
        offset += size
    return result


def inventory(data, index=None):
    available = trees(data)
    if index is None:
        if len(available) != 1:
            raise ValueError(f'{len(available)} trees: choose one with --tree-index')
        index = 0
    if index < 0 or index >= len(available):
        raise ValueError('tree index is out of range')
    offset, size, root = available[index]
    gpu = root.find_by_compatible('qcom,kgsl-3d0')
    if gpu is None:
        raise ValueError('no qcom,kgsl-3d0 node')
    chip = cells(gpu, 'qcom,chipid')
    if len(chip) != 1 or chip[0] >> 16 != 0x0602:
        raise ValueError('selected tree does not describe an Adreno620')
    chip = chip[0]
    all_nodes = list(root.walk())
    phandles = {}
    for node in all_nodes:
        for key in ('phandle', 'linux,phandle'):
            value = cells(node, key)
            if value:
                phandles[value[0]] = node

    def references(node, property_name, cell_property):
        raw, at, values = cells(node, property_name), 0, []
        while at < len(raw):
            provider = phandles.get(raw[at])
            count = cells(provider, cell_property) if provider else []
            if len(count) != 1 or at + 1 + count[0] > len(raw):
                raise ValueError(f'unresolved {property_name} in {node.path()}')
            args = raw[at + 1:at + 1 + count[0]]
            values.append(dict(provider=provider.path(), arguments=[hex(value) for value in args]))
            at += 1 + count[0]
        return values

    def describe(node):
        if node is None:
            return None
        names = strings(node, 'clock-names')
        clocks = references(node, 'clocks', '#clock-cells')
        for position, clock in enumerate(clocks):
            clock['name'] = names[position] if position < len(names) else str(position)
        return dict(path=node.path(), compatible=strings(node, 'compatible'),
                    regions=regions(node), clocks=clocks)

    gmu = root.find_by_compatible('qcom,gpu-gmu')
    gpucc = root.find(lambda node: 'gpucc@' in node.name)
    smmu_nodes = [node for node in all_nodes if node.name.startswith('kgsl-smmu@')]
    if len(smmu_nodes) != 1:
        raise ValueError('expected one GPU SMMU node')
    smmu = smmu_nodes[0]
    routes = []
    for node in all_nodes:
        if node.get('iommus') is None:
            continue
        matches = [entry for entry in references(node, 'iommus', '#iommu-cells')
                   if entry['provider'] == smmu.path()]
        if matches:
            routes.append(dict(path=node.path(), routes=matches))
    reserved = [dict(path=node.path(), regions=regions(node)) for node in all_nodes
                if node.parent and node.parent.name == 'reserved-memory'
                and any(name in node.name for name in ('pil_gpu', 'cont_splash', 'dfps_data'))]
    power = [dict(path=node.path(), name=node.s('regulator-name'), regions=regions(node))
             for node in all_nodes if (node.s('regulator-name') or '').startswith('gpu_')]
    loader = root.find(lambda node: node.s('qcom,firmware-name') == 'a620_zap')
    frequencies = sorted({values[0] for node in gpu.walk()
                          if (values := cells(node, 'qcom,gpu-freq'))})
    return dict(
        schema_version=1,
        evidence='saved device-tree configuration; no hardware access',
        input_sha256=hashlib.sha256(data).hexdigest(),
        tree=dict(index=index, count=len(available), offset=offset, bytes=size,
                  sha256=hashlib.sha256(data[offset:offset + size]).hexdigest()),
        identity=dict(model=root.s('model'), compatible=strings(root, 'compatible'),
                      msm_id=cells(root, 'qcom,msm-id'), board_id=cells(root, 'qcom,board-id')),
        chip_id=f'0x{chip:08x}',
        gmu_chip_word=f'0x{((chip & 0xffff0000) | (((chip >> 8) & 0xff) << 12) | ((chip & 0xff) << 8)):08x}',
        gpu=describe(gpu), gmu=describe(gmu), gpu_clock_controller=describe(gpucc),
        gpu_smmu=describe(smmu), iommu_routes=routes, power_domains=power,
        secure_loader=dict(firmware_name=loader.s('qcom,firmware-name'),
                           pas_id=cells(loader, 'qcom,pas-id')) if loader else None,
        reserved_regions=reserved,
        gpu_frequency_candidates_hz=frequencies,
        unresolved=['firmware bytes and hashes', 'current clock/power state',
                    'runtime speed-bin selection', 'GPU command execution',
                    'active display pipe and physical refresh rate'],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('--tree-index', type=int)
    parser.add_argument('--list-trees', action='store_true', help='list trees before choosing a boot-DTB index')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        data = args.dtb.read_bytes()
        if args.list_trees:
            report = []
            for index, (offset, size, root) in enumerate(trees(data)):
                gpu = root.find_by_compatible('qcom,kgsl-3d0')
                chip = cells(gpu, 'qcom,chipid') if gpu else []
                report.append(dict(index=index, offset=offset, bytes=size,
                                   model=root.s('model'), msm_id=cells(root, 'qcom,msm-id'),
                                   board_id=cells(root, 'qcom,board-id'),
                                   chip_id=f'0x{chip[0]:08x}' if len(chip) == 1 else None))
        else:
            report = inventory(data, args.tree_index)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(2, f'{parser.prog}: {error}\n')
    rendered = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered, encoding='utf-8')
    else:
        print(rendered, end='')


if __name__ == '__main__':
    main()
