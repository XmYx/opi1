#!/bin/sh
# Build the C++ spike: spikes/cpp/build/spike-cpp. Headers come from the local sysroot (sys/setup.sh).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
"$ROOT/sys/setup.sh" >/dev/null
export PKG_CONFIG_PATH="$ROOT/sys/pkgconfig"
mkdir -p "$HERE/build"
CXX="${CXX:-g++}"
# shellcheck disable=SC2046
"$CXX" -std=c++20 -O3 -Wall -Wextra -pthread $(pkg-config --cflags alsa sdl3) \
  "$HERE/spike.cpp" -o "$HERE/build/spike-cpp" $(pkg-config --libs alsa sdl3)
echo "$HERE/build/spike-cpp"
