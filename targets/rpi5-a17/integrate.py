#!/usr/bin/env python3
"""Integrate the snapshotted virtual camera into a pristine Pi 5 AOSP17 tree.

Only the isolated checkout may be modified. Full compile checks are mandatory;
this script does not assert runtime compatibility or change frozen AIDL APIs.
"""
import argparse
import hashlib
import json
import pathlib
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=pathlib.Path, required=True)
parser.add_argument('--source', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
parser.add_argument('--inventory', type=pathlib.Path, required=True)
args = parser.parse_args()
root, source = args.root.resolve(), args.source.resolve()
if root == source or root in source.parents or source in root.parents:
    raise SystemExit('Use a separate AOSP checkout, not the source repository itself')
board = root / 'device/brcm/rpi5'
dest = root / 'hardware/interfaces/camera/provider/virtual'
assert (root / 'build/envsetup.sh').exists()
assert (board / 'BoardConfig.mk').is_file()
marker = dest / '.vcam-rpi5-a17-integration'
if dest.exists() and not marker.exists():
    raise SystemExit('Refusing to overwrite an upstream virtual-camera directory')
dest.mkdir(parents=True, exist_ok=True)
marker.write_text('Pi5 A17 integration from frozen workspace input\n')
touched = set()

def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != content:
        path.write_text(content)
    touched.add(path)

def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or src.read_bytes() != dst.read_bytes():
        shutil.copyfile(src, dst)
    touched.add(dst)

def copy_tree(src, dst):
    for path in sorted(src.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            copy(path, dst / path.relative_to(src))

def append_once(path, block):
    original = path.read_text() if path.exists() else ''
    if block.strip() not in original:
        write(path, original.rstrip() + '\n\n' + block.strip() + '\n')
    else:
        touched.add(path)

def replace_checked(path, old, new):
    text = path.read_text()
    if new in text:
        touched.add(path)
        return
    if text.count(old) != 1:
        raise SystemExit(f'Expected exactly one integration anchor in {path}: {old!r}')
    write(path, text.replace(old, new, 1))

for section in ('stable-aidl', 'platform-jni', 'apex', 'v2-shared-memory'):
    copy_tree(source / section, dest / section)
# A17 makes the external-queue constructor protected and exposes a factory.
# Preserve our existing queue endpoints, usage, acquire count and app-control
# setting; avoid changing lifetime/fence semantics while porting the API.
replace_checked(dest / 'platform-jni/VirtualCameraRelayJni.cpp',
                'queue->consumer = new BufferItemConsumer(consumer, GRALLOC_USAGE_HW_TEXTURE, 2, false);',
                'queue->consumer = BufferItemConsumer::create(consumer, GRALLOC_USAGE_HW_TEXTURE, 2, false);')
for section, target in (('hal/core', 'core'), ('hal/aidl-v1', 'aidl')):
    for path in sorted((source / section).iterdir()):
        if path.suffix in ('.cpp', '.h'):
            copy(path, dest / target / path.name)

# Pi's Mesa/minigbm allocator rejects CAMERA_OUTPUT on RGBA GPU targets.
# This producer is GPU/software, not an ISP; retain the actual GPU_RENDER_TARGET
# or CPU_WRITE_OFTEN access declarations selected below for each output format.
replace_checked(dest / 'aidl/VirtualCameraSession.cpp',
                'int64_t producerUsage = static_cast<int64_t>(BufferUsage::CAMERA_OUTPUT);',
                'int64_t producerUsage = 0; // Pi5: GPU/software producer, not hardware camera output.')

# Use the modern mapper-aware helper, not A13's HIDL-only compatibility shim.
for variant, target in (('core.Android.bp', 'core/Android.bp'),
                        ('aidl-v1.Android.bp', 'aidl/Android.bp')):
    text = (source / 'platform/vendor-variant' / variant).read_text()
    text = text.replace('android.hardware.camera.common@1.0-helper',
                        'android.hardware.camera.common-helper')
    text = text.replace('hardware/interfaces/camera/common/1.0/default/include',
                        'hardware/interfaces/camera/common/default/include')
    text = text.replace('"-DVCAM_COMPAT_A13=1", ', '')
    write(dest / target, text)
for name in ('android.hardware.camera.provider-virtual-service.rc',
             'android.hardware.camera.provider-virtual-service.xml'):
    copy(source / 'platform/vendor-variant' / name, dest / 'aidl' / name)
write(dest / 'Android.bp', '// Pi5 Android17 virtual camera: modules in child directories.\n')
copy_tree(source / 'platform/aidl-lib', dest / 'platform-aidl')
for path in sorted((source / 'aidl').glob('*.aidl')):
    copy(path, dest / 'platform-aidl/android/hardware/virtualcamera' / path.name)
copy(source / 'platform/services/Android.bp', dest / 'platform-service/Android.bp')
for name in ('VirtualCameraService.java', 'VirtualCamera.java', 'VirtualCameraNative.java',
             'LifecycleSlot.java', 'ProducerCapabilities.java'):
    copy(source / 'service' / name,
         dest / 'platform-service/java/com/android/server/camera/virtual' / name)
for app in ('VCamProducer', 'VCamViewer', 'VCamE2E'):
    copy_tree(source / 'platform/apps' / app, dest / 'apps' / app)

# Complete Pi app source: typed foreground service, display-synchronized GL,
# GPU-owned geometry, actual Camera2 split view and the 60fps VGA mode.
copy_tree(pathlib.Path(__file__).resolve().parent / 'apps/VCamProducer',
          dest / 'apps/VCamProducer')

# A17 owns the virtual_camera Binder name for its unrelated built-in camera.
# Keep both brokers and their policies separate; never replace the stock label
# or weaken servicemanager's label validation to hide a namespace collision.
replace_checked(dest / 'platform-service/java/com/android/server/camera/virtual/VirtualCameraService.java',
                'public static final String SERVICE_NAME = "virtual_camera";',
                'public static final String SERVICE_NAME = "ruminate_virtual_camera";')
replace_checked(dest / 'apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java',
                'ServiceManager.getService("virtual_camera")',
                'ServiceManager.getService("ruminate_virtual_camera")')

replace_checked(root / 'frameworks/base/services/Android.bp',
                '"services.core",', '"services.core",\n        "virtual-camera-service",')
server = root / 'frameworks/base/services/java/com/android/server/SystemServer.java'
anchor = '                mSystemServiceManager.startService(CameraServiceProxy.class);'
replacement = anchor + '''

                t.traceBegin("StartVirtualCameraService");
                try {
                    mSystemServiceManager.startService(
                            com.android.server.camera.virtual.VirtualCameraService.Lifecycle.class);
                } catch (Throwable e) {
                    reportWtf("starting VirtualCameraService", e);
                }
                t.traceEnd();'''
replace_checked(server, anchor, replacement)

policy = source / 'platform/sepolicy'
board_policy = board / 'vcam-sepolicy'
for src, target in (
    ('system_ext_public/hal_virtualcamera.te', 'public/hal_virtualcamera.te'),
    ('system_ext_private/virtual_camera_client.te', 'private/virtual_camera_client.te'),
    ('virtual_camera_service.te', 'private/virtual_camera_service.te'),
    ('system_ext_service_contexts', 'private/service_contexts'),
    ('vendor/hal_camera_virtual.te', 'vendor/hal_camera_virtual.te'),
    ('vendor/service_contexts', 'vendor/service_contexts'),
):
    copy(policy / src, board_policy / target)
replace_checked(board_policy / 'private/service_contexts',
                'virtual_camera                            u:object_r:virtual_camera_user_service:s0',
                'ruminate_virtual_camera                   u:object_r:virtual_camera_user_service:s0')
# Android 17 removed the legacy vendor_service attribute. Match current AIDL
# HAL service labels while keeping the vendor-only registration invariant
# explicit; hal_attribute_service below also restricts add to HAL servers.
replace_checked(board_policy / 'vendor/hal_camera_virtual.te',
                '# Service name label for IVirtualCameraHal/default. vendor_service: only a\n'
                '# vendor domain may add it (Treble neverallow). protected_service: untrusted\n'
                '# apps may not even find it.\n'
                'type hal_virtualcamera_service, vendor_service, protected_service, hal_service_type, service_manager_type;',
                '# A17 AIDL HAL service: only HAL servers may register it; untrusted\n'
                '# apps may not find it. Explicitly prohibit core-domain registration.\n'
                'type hal_virtualcamera_service, protected_service, hal_service_type, service_manager_type;\n'
                'neverallow coredomain hal_virtualcamera_service:service_manager add;')
append_once(board / 'BoardConfig.mk', '''# Workspace virtual-camera boundary and broker policy
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/brcm/rpi5/vcam-sepolicy/public
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/brcm/rpi5/vcam-sepolicy/private
BOARD_SEPOLICY_DIRS += device/brcm/rpi5/vcam-sepolicy/vendor
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += device/brcm/rpi5/vcam-framework-matrix.xml''')
copy(source / 'platform/vintf/device_framework_matrix_virtualcamera.xml',
     board / 'vcam-framework-matrix.xml')
# A13 needed an extra vendor manifest fragment because it did not discover
# vendor-APEX VINTF fragments. A17 discovers them: shipping both declarations
# makes the combined device manifest invalid. Remove only the legacy package.
legacy_manifest_package = '    android.hardware.camera.provider-virtual-service.vintf \\\n'
device_text = (board / 'device.mk').read_text()
if legacy_manifest_package in device_text:
    assert device_text.count(legacy_manifest_package) == 1
    write(board / 'device.mk', device_text.replace(legacy_manifest_package, '', 1))
append_once(board / 'device.mk', '''# Workspace virtual camera: one vendor APEX, matched platform JNI and apps
PRODUCT_PACKAGES += \\
    com.android.hardware.camera.provider.virtual \\
    libvirtualcamera_relay_jni \\
    VCamProducer \\
    VCamProducer2 \\
    VCamViewer
''')
# E2E is intentionally built separately for data-installation as an ordinary
# public-SDK app; baking it into /system would weaken that qualification gate.
# Upstream CI test packager assumes OUT_DIR starts with the literal "out".
# Filter by the actual product root, so absolute OUT_DIR also skips host tools.
replace_checked(root / 'build/soong/ci_tests/ci_test_package_zip.go',
                '\t\tf := strings.TrimPrefix(installedFile.String(), productOut+"/")\n'
                '\t\tif strings.HasPrefix(f, "out") {',
                '\t\tf, inProduct := strings.CutPrefix(installedFile.String(), productOut+"/")\n'
                '\t\tif !inProduct {')

# rustc_wrapper strips the source-root prefix from depfile paths, including
# absolute OUT_DIR paths. Match that normalization for expected suffixes too;
# never bypass undeclared-input verification or accept arbitrary basenames.
replace_checked(root / 'build/soong/cmd/depfile_verifier/main.go',
                '\thasFailures := false\n',
                '\tcwd, err := os.Getwd()\n'
                '\tif err != nil {\n'
                '\t\tlog.Fatalf("Failed to get working directory: %v", err)\n'
                '\t}\n\n'
                '\thasFailures := false\n')
replace_checked(root / 'build/soong/cmd/depfile_verifier/main.go',
                '\t\t\t\tif strings.HasSuffix(input, expectedFile) {',
                '\t\t\t\t// Match rustc_wrapper source-root normalization for absolute OUT_DIR.\n'
                '\t\t\t\texpectedFile = strings.TrimPrefix(expectedFile, cwd+string(filepath.Separator))\n'
                '\t\t\t\tif strings.HasSuffix(input, expectedFile) {')

inventory = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in sorted(touched)}
args.inventory.write_text(json.dumps(inventory, indent=2, sort_keys=True) + '\n')
print(f'Integrated {len(inventory)} files; compilation and runtime checks remain pending.')
