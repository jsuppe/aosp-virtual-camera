#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
test_binary="$(mktemp /tmp/vcam-fence-test.XXXXXXXX)"
trap 'rm -f "$test_binary"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$project_root/hal/core" \
    "$project_root/tests/AcquireFenceWaitTest.cpp" -o "$test_binary"
"$test_binary"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$project_root/hal/core" \
    "$project_root/tests/ReadFenceSetTest.cpp" -o "$test_binary"
"$test_binary"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$project_root/hal/core" \
    "$project_root/tests/ReadFenceFdTest.cpp" -o "$test_binary"
"$test_binary"
