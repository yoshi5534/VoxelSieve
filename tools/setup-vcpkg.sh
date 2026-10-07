#!/usr/bin/env bash
# Clones vcpkg into $VCPKG_ROOT (default ~/vcpkg) at the builtin-baseline of vcpkg.json and
# bootstraps it, so that the CMake presets find it. Safe to run again: it only fetches and checks
# out the pinned commit.
set -euo pipefail
cd "$(dirname "$0")/.."
root="${VCPKG_ROOT:-$HOME/vcpkg}"
baseline="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' vcpkg.json)"
if [[ ! -d "$root/.git" ]]; then
  git clone --quiet https://github.com/microsoft/vcpkg "$root"
fi
git -C "$root" fetch --quiet origin "$baseline" 2>/dev/null || git -C "$root" fetch --quiet origin
git -C "$root" -c advice.detachedHead=false checkout --quiet "$baseline"
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)  # Git Bash on Windows
    if [[ ! -x "$root/vcpkg.exe" ]]; then
      cmd //c "$(cygpath -w "$root/bootstrap-vcpkg.bat")" -disableMetrics
    fi
    ;;
  *)
    if [[ ! -x "$root/vcpkg" ]]; then
      "$root/bootstrap-vcpkg.sh" -disableMetrics
    fi
    ;;
esac
echo "vcpkg $baseline in $root; export VCPKG_ROOT=$root before cmake --preset"
