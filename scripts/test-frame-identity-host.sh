#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d /tmp/vcam-identity.XXXXXXXX)"
trap 'rm -f "$test_dir/com/example/vcamtest/"*.class; rmdir "$test_dir/com/example/vcamtest" "$test_dir/com/example" "$test_dir/com" "$test_dir"' EXIT
javac -Xlint:all -Werror -d "$test_dir" \
    "$project_root/platform/apps/VCamE2E/src/com/example/vcamtest/FrameIdentity.java" \
    "$project_root/tests/FrameIdentityTest.java"
java -ea -cp "$test_dir" com.example.vcamtest.FrameIdentityTest
