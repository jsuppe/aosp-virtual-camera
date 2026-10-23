#!/usr/bin/env python3
"""Assemble the upstream Pi5 MBR layout without loop devices or root access."""
import argparse
import hashlib
import json
import os
import pathlib
import struct
import subprocess

SECTOR = 512
MIB_SECTORS = 2048
IMAGE_BYTES = 15360000000

def layout():
    boot_start, boot_size = 2048, 128 * MIB_SECTORS
    extended_start = boot_start + boot_size
    extended_size = (3072 + 384 + 16 + 4) * MIB_SECTORS
    system_start = extended_start + MIB_SECTORS
    system_size = 3072 * MIB_SECTORS
    vendor_start = system_start + system_size + MIB_SECTORS
    vendor_size = 384 * MIB_SECTORS
    metadata_start = vendor_start + vendor_size + MIB_SECTORS
    userdata_start = extended_start + extended_size
    return {
        1: (boot_start, boot_size, 'c'),
        2: (extended_start, extended_size, '5'),
        3: (userdata_start, IMAGE_BYTES // SECTOR - userdata_start, '83'),
        5: (system_start, system_size, '83'),
        6: (vendor_start, vendor_size, '83'),
        7: (metadata_start, 16 * MIB_SECTORS, '83'),
    }

def digest_range(path, offset, size):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        stream.seek(offset)
        while size:
            chunk = stream.read(min(size, 4 * 1024 * 1024))
            if not chunk:
                raise ValueError(f'Short read: {path}')
            digest.update(chunk)
            size -= len(chunk)
    return digest.hexdigest()

def create_table(image):
    if image.exists():
        raise FileExistsError(image)
    with image.open('xb') as stream:
        stream.truncate(IMAGE_BYTES)
    script = 'label: dos\nunit: sectors\n\n'
    for number, (start, size, kind) in layout().items():
        script += f'{number}: start={start}, size={size}, type={kind}'
        script += ', bootable\n' if number == 1 else '\n'
    subprocess.run(['/usr/sbin/sfdisk', '--no-reread', '--no-tell-kernel',
                    '--wipe', 'never', '--wipe-partitions', 'never', str(image)],
                   input=script, text=True, check=True)
    check_table(image)

def check_table(image):
    data = json.loads(subprocess.check_output(['/usr/sbin/sfdisk', '--json', str(image)]))
    table = data['partitiontable']
    assert table['label'] == 'dos'
    actual = {int(row['node'][len(str(image)):]): row for row in table['partitions']}
    assert set(actual) == set(layout())
    for number, (start, size, kind) in layout().items():
        assert (actual[number]['start'], actual[number]['size'], actual[number]['type']) == (start, size, kind)
    assert actual[1].get('bootable') is True
    assert image.stat().st_size == IMAGE_BYTES
    with image.open('rb') as stream:
        stream.seek(510)
        assert stream.read(2) == b'\x55\xaa'
    return data

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--product-out', type=pathlib.Path, required=True)
    parser.add_argument('--image', type=pathlib.Path, required=True)
    parser.add_argument('--report', type=pathlib.Path, required=True)
    args = parser.parse_args()
    partitions = {'boot': 1, 'system': 5, 'vendor': 6}
    inputs = {}
    for name, number in partitions.items():
        source = args.product_out / f'{name}.img'
        assert source.is_file(), source
        with source.open('rb') as stream:
            assert stream.read(4) != struct.pack('<I', 0xED26FF3A), 'Convert Android sparse images before packaging'
        assert 0 < source.stat().st_size <= layout()[number][1] * SECTOR
        inputs[name] = {'path': str(source), 'bytes': source.stat().st_size,
                        'sha256': digest_range(source, 0, source.stat().st_size)}
    create_table(args.image)
    with args.image.open('r+b') as output:
        for name, number in partitions.items():
            output.seek(layout()[number][0] * SECTOR)
            with (args.product_out / f'{name}.img').open('rb') as source:
                while chunk := source.read(4 * 1024 * 1024):
                    # Preserve sparse zero ranges in this newly created image.
                    if chunk.count(0) == len(chunk):
                        output.seek(len(chunk), os.SEEK_CUR)
                    else:
                        output.write(chunk)
        output.flush()
        os.fsync(output.fileno())
    for name, number in (('metadata', 7), ('userdata', 3)):
        start, size, _ = layout()[number]
        subprocess.run(['/usr/sbin/mkfs.ext4', '-F', '-b', '4096', '-I', '512',
                        '-L', name, '-E', f'offset={start * SECTOR},nodiscard',
                        str(args.image), str(size * SECTOR // 4096)], check=True)
    table = check_table(args.image)
    for name, number in partitions.items():
        packaged = digest_range(args.image, layout()[number][0] * SECTOR, inputs[name]['bytes'])
        assert packaged == inputs[name]['sha256'], f'Packaged {name} bytes differ'
    with args.image.open('rb') as stream:
        for number in (3, 5, 6, 7):
            stream.seek(layout()[number][0] * SECTOR + 1024 + 56)
            assert stream.read(2) == b'\x53\xef', f'No ext4 superblock on partition {number}'
    report = {'image': str(args.image), 'bytes': IMAGE_BYTES,
              'sha256': digest_range(args.image, 0, IMAGE_BYTES),
              'inputs': inputs, 'partition_table': table,
              'checks': ['MBR boot signature', 'upstream partition numbering and bounds',
                         'bootable FAT partition flag', 'ext4 superblock signatures',
                         'boot/system/vendor byte-for-byte hashes after filesystem creation'],
              'pi5_boot_test': 'NOT_RUN', 'camera_runtime_test': 'NOT_RUN'}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('IMAGE_ASSEMBLY_VERIFIED; actual Pi 5 boot and camera qualification NOT RUN.')

if __name__ == '__main__':
    main()
