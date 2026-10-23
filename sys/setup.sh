#!/bin/sh
# Local build sysroot for op1-og-native: the ALSA and SDL3 development headers plus link names for the
# runtime libraries every Ubuntu desktop already has (libasound.so.2, libSDL3.so.0). No root, no apt
# install: `apt-get download` fetches the -dev packages that match the installed runtime versions and
# `dpkg-deb -x` unpacks them here. Everything this writes is under sys/root, sys/lib and sys/pkgconfig
# (gitignored). Safe to re-run.
#
#   sys/setup.sh            # set up (skips if already done for the same versions)
#   OP1_SYS_FORCE=1 sys/setup.sh
#
# Then:  export PKG_CONFIG_PATH="$PWD/sys/pkgconfig"   (the build scripts do this themselves)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
TRIPLET="$(gcc -dumpmachine 2>/dev/null || echo "$(uname -m)-linux-gnu")"
STAMP="$HERE/.setup-done"

find_rt() { # $1 = soname
  for d in "/usr/lib/$TRIPLET" /usr/lib64 /usr/lib "/lib/$TRIPLET"; do
    [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }
  done
  return 1
}

asound_so="$(find_rt libasound.so.2)" || { echo "libasound.so.2 not found (install libasound2t64)" >&2; exit 1; }
sdl3_so="$(find_rt libSDL3.so.0)" || { echo "libSDL3.so.0 not found (install libsdl3-0)" >&2; exit 1; }

# dev package versions = installed runtime versions, so headers and library agree
ver() { dpkg-query -W -f='${Version}' "$1" 2>/dev/null || true; }
asound_ver="$(ver "libasound2t64:$ARCH")"; [ -n "$asound_ver" ] || asound_ver="$(ver "libasound2:$ARCH")"
sdl3_ver="$(ver "libsdl3-0:$ARCH")"
want="asound=$asound_ver sdl3=$sdl3_ver"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ] && [ -z "${OP1_SYS_FORCE:-}" ]; then
  echo "sys: up to date ($want)"; exit 0
fi

dl="$(mktemp -d)"; trap 'rm -rf "$dl"' EXIT
(
  cd "$dl"
  if [ -n "$asound_ver" ]; then apt-get download "libasound2-dev=$asound_ver" >/dev/null; else apt-get download libasound2-dev >/dev/null; fi
  if [ -n "$sdl3_ver" ]; then apt-get download "libsdl3-dev=$sdl3_ver" >/dev/null; else apt-get download libsdl3-dev >/dev/null; fi
)
rm -rf "$HERE/root" "$HERE/lib" "$HERE/pkgconfig"
mkdir -p "$HERE/root" "$HERE/lib" "$HERE/pkgconfig"
for deb in "$dl"/*.deb; do dpkg-deb -x "$deb" "$HERE/root"; done

ln -sfn "$asound_so" "$HERE/lib/libasound.so"
ln -sfn "$sdl3_so" "$HERE/lib/libSDL3.so"

inc="$HERE/root/usr/include"
cat > "$HERE/pkgconfig/alsa.pc" <<PC
prefix=$HERE
libdir=$HERE/lib
includedir=$inc
Name: alsa
Description: ALSA (local sysroot for op1-og-native)
Version: ${asound_ver%%-*}
Libs: -L\${libdir} -lasound
Cflags: -I\${includedir}
PC
cat > "$HERE/pkgconfig/sdl3.pc" <<PC
prefix=$HERE
libdir=$HERE/lib
includedir=$inc
Name: sdl3
Description: SDL3 (local sysroot for op1-og-native)
Version: ${sdl3_ver%%+*}
Libs: -L\${libdir} -lSDL3
Cflags: -I\${includedir}
PC
echo "$want" > "$STAMP"
echo "sys: ready ($want) -> $HERE"
