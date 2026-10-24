#!/usr/bin/env python3
"""Check the combined servicemanager label namespace across shipped partitions."""
import pathlib
import sys

product = pathlib.Path(sys.argv[1])
paths = [product / path for path in (
    'system/etc/selinux/plat_service_contexts',
    'system/system_ext/etc/selinux/system_ext_service_contexts',
    'system/product/etc/selinux/product_service_contexts',
    'vendor/etc/selinux/vendor_service_contexts',
    'vendor/odm/etc/selinux/odm_service_contexts',
)]
labels = {}
errors = []
for path in paths:
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split('#', 1)[0].split()
        if not line:
            continue
        name, label = line
        previous = labels.get(name)
        if previous and previous[0] != label:
            errors.append(f'{name}: {previous} conflicts with {(label, str(path), line_number)}')
        labels[name] = (label, str(path), line_number)
if errors:
    print('\n'.join(errors))
    raise SystemExit(1)
assert labels['virtual_camera'][0] == 'u:object_r:virtual_camera_service:s0'
assert labels['ruminate_virtual_camera'][0] == 'u:object_r:virtual_camera_user_service:s0'
print(f'PASS: {len(labels)} combined service labels; stock/custom brokers distinct.')
