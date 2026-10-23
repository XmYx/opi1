#!/bin/sh
# Builds the t7 measurement harness against the objects of an existing ./build.sh build (links like op1-og).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
B="$ROOT/build/CMakeFiles"
OBJS=$(find "$B"/op1_*.dir -name '*.o' | tr '\n' ' ')
# shellcheck disable=SC2086
nice -n 19 c++ -std=c++20 -O2 -g -I"$ROOT/include" "$HERE/measure.cpp" $OBJS -o "$HERE/measure" \
  -Wl,-rpath,"$ROOT/sys/lib" "$ROOT/sys/lib/libasound.so" "$ROOT/sys/lib/libSDL3.so" -lpthread
echo "$HERE/measure"
nice -n 19 cc -O2 -fPIC -shared -I"$ROOT/sys/root/usr/include" "$HERE/uishim.c" -o "$HERE/uishim.so" -ldl -lpthread
echo "$HERE/uishim.so"
