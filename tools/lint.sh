#!/usr/bin/env bash
# Runs clang-format and clang-tidy the same way CI does.
# Usage: tools/lint.sh [build-dir]   (default: build/debug, configured with `cmake --preset debug`)
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build/debug}"

clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')

# OpenVDB and Boost headers make every translation unit large; clang-tidy analyses them and then
# suppresses the findings, which is where the time goes. Running files in parallel keeps this
# manageable, and the "N warnings generated" lines about suppressed header findings are dropped.
set +o pipefail
run-clang-tidy -p "$build_dir" -quiet -j "$(nproc)" "$(pwd)/(src|apps|tests)/" 2>&1 |
  grep -v -E '^[0-9]+ warnings? (and [0-9]+ errors? )?generated\.$|^clang-tidy.* -p=' |
  tee /tmp/voxelsieve-clang-tidy.log
status=${PIPESTATUS[0]}
exit "$status"
