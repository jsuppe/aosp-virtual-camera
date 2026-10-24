#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d /tmp/vcam-render-mailbox.XXXXXXXX)"
trap 'rm -f "$test_dir/com/example/vcamproducer/"*.class; rmdir "$test_dir/com/example/vcamproducer" "$test_dir/com/example" "$test_dir/com" "$test_dir"' EXIT
javac -Xlint:all -Werror -d "$test_dir" \
    "$project_root/platform/apps/VCamProducer/src/com/example/vcamproducer/RenderRequestQueue.java" \
    "$project_root/tests/RenderRequestQueueTest.java"
java -ea -cp "$test_dir" com.example.vcamproducer.RenderRequestQueueTest
