#!/bin/sh
# Build op1-og-native (C++20, CMake). No root needed: sys/setup.sh fetches the ALSA and SDL3 headers.
#
#   ./build.sh              Release build -> build/op1-og
#   ./build.sh test         build, then run every test (ctest)
#   ./build.sh sanitize     AddressSanitizer + UBSan build in build-sanitize/, then the tests
#   JOBS=N ./build.sh       parallel compile jobs (default 8; magix-system runs close to its memory limit)
#   The gui tests render the screens only with the user's firmware image: OP1_OG_FIRMWARE, else the first of
#   ./firmware/op1_*.op1 and ~/rigs-work/op1-og-fw/fw/op1_*.op1 (as run.sh). Without one they say SKIPPED.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
"$HERE/sys/setup.sh" >/dev/null
PKG_CONFIG_PATH="$HERE/sys/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH
mode="${1:-release}"
case "$mode" in
  release|test) dir="$HERE/build"; type=Release; san=OFF ;;
  sanitize) dir="$HERE/build-sanitize"; type=Debug; san=ON ;;
  *) sed -n '2,8p' "$0"; exit 64 ;;
esac
cmake -S "$HERE" -B "$dir" -DCMAKE_BUILD_TYPE="$type" -DOP1_SANITIZE="$san" >/dev/null
cmake --build "$dir" -j "${JOBS:-8}"
if [ "$mode" != release ]; then
  if [ -z "${OP1_OG_FIRMWARE:-}" ]; then
    for f in "$HERE"/firmware/op1_*.op1 "$HOME"/rigs-work/op1-og-fw/fw/op1_*.op1; do
      if [ -f "$f" ]; then OP1_OG_FIRMWARE="$f"; export OP1_OG_FIRMWARE; break; fi
    done
  fi
  echo "build.sh: gui tests with firmware: ${OP1_OG_FIRMWARE:-none (SKIPPED parts)}" >&2
  ctest --test-dir "$dir" --output-on-failure
fi
echo "$dir/op1-og"
