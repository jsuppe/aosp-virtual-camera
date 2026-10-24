#!/usr/bin/env python3
"""A17 discovers vendor APEX manifests; reject the redundant A13 fallback."""
import pathlib
import subprocess
import sys

product = pathlib.Path(sys.argv[1])
host = pathlib.Path(sys.argv[2])
legacy = product / 'vendor/etc/vintf/manifest/android.hardware.camera.provider-virtual-service.xml'
assert not legacy.exists(), f'Duplicate A13 fallback manifest still installed: {legacy}'
apex = product / 'vendor/apex/com.android.hardware.camera.provider.virtual.apex'
listing = subprocess.check_output([
    str(host / 'deapexer'), '--debugfs_path', str(host / 'debugfs_static'),
    'list', str(apex),
], text=True)
assert './etc/vintf/com.android.hardware.camera.provider.virtual.xml' in listing.splitlines()
print('PASS: A17 camera VINTF fragment present inside APEX, redundant vendor fallback absent.')
