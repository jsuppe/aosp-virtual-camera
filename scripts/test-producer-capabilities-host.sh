#!/bin/bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
task_temp="$(mktemp -d)"
trap 'rm -rf "$task_temp"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -I "$repo_root/hal/core" "$repo_root/tests/ProducerCapabilitiesTest.cpp" -o "$task_temp/capabilities"
"$task_temp/capabilities"
