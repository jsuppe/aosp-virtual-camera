#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d /tmp/vcam-lifecycle-slot.XXXXXXXX)"
trap 'rm -f "$test_dir/com/android/server/camera/virtual/"*.class; rmdir "$test_dir/com/android/server/camera/virtual" "$test_dir/com/android/server/camera" "$test_dir/com/android/server" "$test_dir/com/android" "$test_dir/com" "$test_dir"' EXIT
javac -Xlint:all -Werror -d "$test_dir" \
    "$project_root/service/LifecycleSlot.java" "$project_root/tests/LifecycleSlotTest.java"
java -ea -cp "$test_dir" com.android.server.camera.virtual.LifecycleSlotTest
