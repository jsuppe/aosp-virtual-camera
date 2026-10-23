#!/bin/bash
# Run against a separate, matched Android17 Raspberry Vanilla checkout.
set -eo pipefail
target_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$target_dir/../.." && pwd)"
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 AOSP_ROOT [EVIDENCE_DIR]" >&2
    exit 2
fi
aosp_root="$(cd "$1" && pwd)"
evidence="${2:-$aosp_root/vcam-build-evidence}"
mkdir -p "$evidence"
evidence="$(cd "$evidence" && pwd)"
exec > >(tee -a "$evidence/build.log") 2>&1
trap 'result=$?; printf "%s\n" "$result" > "$evidence/build.exit"' EXIT
export OUT_DIR="${OUT_DIR:-$aosp_root/out}"
export SOONG_NINJA=ninja
export PATH="${VCAM_HOST_TOOLS_PATH:-/usr/bin}:/usr/sbin:/sbin:$PATH"
for tool in python3 mkfs.fat mcopy; do command -v "$tool" >/dev/null; done
python3 "$target_dir/integrate.py" --root "$aosp_root" --source "$source_root" --inventory "$evidence/integration-first.json"
python3 "$target_dir/integrate.py" --root "$aosp_root" --source "$source_root" --inventory "$evidence/integration-repeat.json"
cmp "$evidence/integration-first.json" "$evidence/integration-repeat.json"
cd "$aosp_root"
repo diff > "$evidence/integration.patch"
source build/envsetup.sh
lunch aosp_rpi5-cp2a-userdebug
test "$(build/soong/soong_ui.bash --dumpvar-mode TARGET_ARCH)" = arm64
m -j"${JOBS:-8}" bootimage systemimage vendorimage VCamE2E
python3 "$target_dir/check-service-labels.py" "$OUT_DIR/target/product/rpi5"
python3 "$target_dir/check-vintf-placement.py" "$OUT_DIR/target/product/rpi5" "$OUT_DIR/host/linux-x86/bin"
echo 'Image build complete; package and qualify the actual image separately.'
