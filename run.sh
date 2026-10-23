#!/usr/bin/env bash
# Launcher for the OG OP-1 native port (owner: t6). Works like the Pi launcher (pi/op1-field-pi.sh +
# adopt_desktop_session() of pi/image/run_local.sh): from SSH, tmux or a TTY it finds the logged-in
# desktop session by itself, so the window opens on that screen. It builds first if needed.
#
#   ./run.sh [options] [-- op1-og options]
#
#   (none)             the OP-1 display only
#   --keyboard         also show the on-screen keyboard and the key-mapping legend (Ctrl+K hides the legend)
#   --firmware PATH    your OG firmware image (default: $OP1_OG_FIRMWARE, ./firmware/op1_*.op1, then
#                      ~/rigs-work/op1-og-fw/fw/op1_*.op1). Without one the screens are not drawn.
#   --scale N          integer window scale of the 320x160 display (default 3)
#   --fullscreen       start fullscreen (Ctrl+F toggles)
#   --duration S       exit after S seconds
#   --screenshot FILE  save the window as BMP after about 1.5 s
#   --screen NAME      start on one firmware screen (e.g. tape, fm, mixer; Ctrl+[ ] browse all of them)
#   --no-build         do not (re)build, fail if build/op1-og is missing
#   --device DEV       audio device: pipewire, default, hw:X,Y, none, or wav:FILE (render to a WAV file).
#                      Default: $OP1_OG_DEVICE, else pipewire on a desktop session (the output picked in the
#                      desktop's sound settings), else hw:0,0. If it cannot be opened, ALSA "default" is tried.
#   --check            print what would be used (session, firmware, binary, audio device) and exit
#   -h, --help         this text
# Any other option is passed on to op1-og (see build/op1-og --help).
#
# Keys (by physical position, any layout; the PC keyboard is laid out like the OG panel, KEYMAP.md):
#   F1 F2 F3 F4        push the blue, green, white, orange encoder
#   F5/F6 F7/F8 F9/F10 F11/F12   turn blue, green, white, orange -/+ (Ctrl = 8 steps; mouse wheel = last one)
#   ` help   1 synth  2 drum  3 tape  4 mixer  5 tempo   = input (mic)   Backspace com   Tab or \ sequencer
#   Q W E R            T1 T2 T3 T4
#   A S D              lift  drop  split          (Up/Down also lift/drop)
#   Z X C              record  play  stop         (Enter also record, Space also play)
#                      shift+Z = record overdub; Z held (or armed) + a note starts recording
#   Shift + Ctrl+1..8  preset browser: blue folder (engine), green preset (previews), encoder push = keep on slot
#   Left/Right arrows  left/right: octave -/+ in synth and drum, tape back/forward elsewhere
#   Shift              shift
#   musical keys F..E  V B N M , . /  (black: G H J L ;)  then  T Y U I O P [  (black: 6 7 8 0 -)
#   Ctrl+1..8, keypad 1-8   sound keys 1-8 (tape and mixer: tape tricks)
#   PageUp/PageDown    volume knob      End   power switch
#   Ctrl+K legend  Ctrl+F fullscreen  Esc quit  Ctrl+[ ] \ browse all screens  Ctrl+, Ctrl+. engine
#   Ctrl+A album  Ctrl+B preset browser  Ctrl+C clock  Ctrl+T tune  Ctrl+G signal flow  Ctrl+L lander
#
# Desktop session: when neither DISPLAY nor WAYLAND_DISPLAY is set, the script takes the user's
# XDG_RUNTIME_DIR (/run/user/UID), the session bus, the first wayland-N socket and the DISPLAY and
# XAUTHORITY of the user's Xwayland/Xorg process. OP1_OG_SESSION=desktop forces this even if DISPLAY is
# set (for example by SSH X forwarding); OP1_OG_SESSION=none skips it (headless, no window).
# Exit status: 0 ok, 1 audio device could not be opened, 64 usage, 69 no graphical session, 70 build failed.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

usage() { sed -n '2,/^set -uo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
die() { echo "run.sh: $*" >&2; exit "${2:-1}"; }

keyboard=0 build=1 check=0 firmware="${OP1_OG_FIRMWARE:-}" device_given=0 fullscreen=0 device=""
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --keyboard) keyboard=1 ;;
    --no-build) build=0 ;;
    --check) check=1 ;;
    --fullscreen) fullscreen=1 ;;
    --firmware) [ $# -ge 2 ] || die "--firmware needs a path" 64; firmware="$2"; shift ;;
    --screen) [ $# -ge 2 ] || die "--screen needs a name" 64; export OP1_OG_SCREEN="$2"; shift ;;
    --scale) [ $# -ge 2 ] || die "--scale needs a number" 64; export OP1_OG_SCALE="$2"; shift ;;
    --duration|--screenshot|--stats|--period|--fill|--midi-name)
      [ $# -ge 2 ] || die "$1 needs a value" 64; args+=("$1" "$2"); shift ;;
    --device) [ $# -ge 2 ] || die "--device needs a value" 64; args+=("$1" "$2"); device_given=1; device="$2"; shift ;;
    --) shift; args+=("$@"); break ;;
    *) args+=("$1") ;;
  esac
  shift
done

# ---------------------------------------------------------------- desktop session (from pi/image/run_local.sh)
adopt_desktop_session() {
  local rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" s pid a
  local -a argv
  [ -d "$rt" ] || return 1
  export XDG_RUNTIME_DIR="$rt"
  if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -S "$rt/bus" ]; then export DBUS_SESSION_BUS_ADDRESS="unix:path=$rt/bus"; fi
  if [ "${OP1_OG_SESSION:-}" = desktop ]; then unset DISPLAY WAYLAND_DISPLAY XAUTHORITY; fi
  if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for s in "$rt"/wayland-[0-9]*; do
      case "$s" in *.lock) continue ;; esac
      if [ -S "$s" ]; then export WAYLAND_DISPLAY="${s##*/}"; break; fi
    done
  fi
  if [ -z "${DISPLAY:-}" ]; then
    for pid in $(pgrep -u "$(id -u)" -x 'Xwayland|Xorg|X' 2>/dev/null || true); do
      mapfile -d '' -t argv < "/proc/$pid/cmdline" 2>/dev/null || continue
      for ((a = 1; a < ${#argv[@]}; a++)); do
        case "${argv[a]}" in
          :[0-9]*) [ -n "${DISPLAY:-}" ] || export DISPLAY="${argv[a]}" ;;
          -auth) if [ -z "${XAUTHORITY:-}" ] && [ -f "${argv[a+1]:-}" ]; then export XAUTHORITY="${argv[a+1]}"; fi ;;
        esac
      done
      if [ -n "${DISPLAY:-}" ]; then break; fi
    done
  fi
  [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]
}

session_note=""
if [ "${OP1_OG_SESSION:-}" = none ]; then
  args+=(--no-window)
  session_note="headless (OP1_OG_SESSION=none)"
elif { [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; } || [ "${OP1_OG_SESSION:-}" = desktop ]; then
  adopt_desktop_session || die "no graphical session for user $(id -un): nobody is logged in on this machine's screen (no Wayland socket or X server under /run/user/$(id -u)). Log in there first, or set OP1_OG_SESSION=none" 69
  session_note="adopted desktop session: ${WAYLAND_DISPLAY:+WAYLAND_DISPLAY=$WAYLAND_DISPLAY }${DISPLAY:+DISPLAY=$DISPLAY}"
else
  session_note="using the session already set: ${WAYLAND_DISPLAY:+WAYLAND_DISPLAY=$WAYLAND_DISPLAY }${DISPLAY:+DISPLAY=$DISPLAY}"
fi

# ---------------------------------------------------------------- firmware image
if [ -z "$firmware" ]; then
  for f in "$HERE"/firmware/op1_*.op1 "$HOME"/rigs-work/op1-og-fw/fw/op1_*.op1; do
    if [ -f "$f" ]; then firmware="$f"; break; fi
  done
fi
if [ -n "$firmware" ] && [ ! -f "$firmware" ]; then die "firmware image not found: $firmware" 64; fi

# ---------------------------------------------------------------- build
bin="$HERE/build/op1-og"
needs_build() {
  [ -x "$bin" ] || return 0
  [ -n "$(find "$HERE/src" "$HERE/include" "$HERE/CMakeLists.txt" -newer "$bin" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name CMakeLists.txt \) -print -quit 2>/dev/null)" ]
}
if [ "$build" = 1 ] && needs_build; then
  echo "run.sh: building ..." >&2
  "$HERE/build.sh" >&2 || die "build failed (see above)" 70
fi
[ -x "$bin" ] || die "$bin is missing (run ./build.sh, or drop --no-build)" 70

# ---------------------------------------------------------------- audio device (same rule as op1-og)
if [ -z "$device" ]; then
  if [ -n "${OP1_OG_DEVICE:-}" ]; then device="$OP1_OG_DEVICE"
  elif [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pipewire-0" ]; then device=pipewire
  else device="hw:0,0"; fi
  args+=(--device "$device")
fi
# A short PipeWire quantum for the pipewire device (as native/run.sh does); the user's own value wins.
if [ "$device" = pipewire ] && [ -z "${PIPEWIRE_LATENCY:-}" ]; then export PIPEWIRE_LATENCY=64/44100; fi

echo "run.sh: $session_note" >&2
echo "run.sh: firmware: ${firmware:-none (screens will not be drawn)}" >&2
echo "run.sh: audio: $device${PIPEWIRE_LATENCY:+ (PIPEWIRE_LATENCY=$PIPEWIRE_LATENCY)}" >&2
if [ "$check" = 1 ]; then
  echo "run.sh: binary: $bin"; echo "run.sh: overlay: $([ $keyboard = 1 ] && echo keyboard+legend || echo off)"
  exit 0
fi

[ "$fullscreen" = 1 ] && export OP1_OG_FULLSCREEN=1
[ "$keyboard" = 1 ] && args+=(--keyboard)
[ -n "$firmware" ] && args+=(--firmware "$firmware")

# SDL3 from the project-local copy when the system has none (build.sh keeps it in sys/lib)
[ -d "$HERE/sys/lib" ] && export LD_LIBRARY_PATH="$HERE/sys/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$bin" "${args[@]}"
rc=$?
if [ $rc -eq 1 ] && [ $device_given = 0 ] && [ -z "${OP1_OG_DEVICE:-}" ] && [ "$device" != default ]; then
  echo "run.sh: $device could not be opened, retrying with the ALSA default device" >&2
  "$bin" "${args[@]}" --device default
  rc=$?
fi
exit $rc
