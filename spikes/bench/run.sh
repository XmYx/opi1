#!/usr/bin/env bash
# Runs the t3 measurements for both spikes. Raw results go to $OUT (default: the project's shared folder,
# ~/RIGS-Shared/files/op1-og-native/results/language-spikes); bench/analyze.py turns them into tables.
#
#   bench/run.sh prep                 build both spikes and the MIDI probe
#   bench/run.sh build                clean/incremental build times x3, binary sizes
#   bench/run.sh dsp                  offline DSP bench, pinned to a P-core (2) and an E-core (20), x3
#   bench/run.sh sweep                period x fill grid on $DEV, $SWEEP_S s each, $REPS (2) times, no window
#                                     (CELLS="32 3,64 4" FROM_REP=2 REPS=2 runs a subset)
#   bench/run.sh soak P FILL          $SOAK_S s with the window and the MIDI probe (xruns, latency, UI, CPU)
#   bench/run.sh pipewire             the desktop path: ALSA "pipewire" device, muted, with the MIDI probe
#   bench/run.sh shots                one window screenshot per spike
#
# Audio runs use $DEV (default hw:0,0, the onboard codec: nothing is plugged in, so the runs are silent) at
# 44.1 kHz with realtime priority from rtkit. soak and shots open the spike window on the desktop session.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SPK="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SPK/.." && pwd)"
SHARED="${RIGS_SHARED:-$HOME/RIGS-Shared}/files/op1-og-native"
OUT="${OUT:-$SHARED/results/language-spikes}"
DEV="${DEV:-hw:0,0}"
SWEEP_S="${SWEEP_S:-20}"
SOAK_S="${SOAK_S:-600}"
PW_S="${PW_S:-60}"
CPP="$SPK/cpp/build/spike-cpp"
RUST="$SPK/rust/target/release/spike-rust"
PROBE="$HERE/build/midi_probe"
mkdir -p "$OUT"

bin_of() { if [ "$1" = cpp ]; then echo "$CPP"; else echo "$RUST"; fi; }
us() { echo "${EPOCHREALTIME/./}"; }  # wall clock in microseconds
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", (b - a) / 1e6 }'; }

adopt_session() {  # same rules as adopt_desktop_session() in pi/image/run_local.sh
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for s in "$XDG_RUNTIME_DIR"/wayland-[0-9]*; do
      [ -S "$s" ] && { export WAYLAND_DISPLAY="${s##*/}"; break; }
    done
  fi
  if [ -z "${DISPLAY:-}" ]; then
    local pid d
    pid="$(pgrep -u "$(id -u)" -x 'Xwayland|Xorg|X' | head -1 || true)"
    if [ -n "$pid" ]; then
      d="$(tr '\0' '\n' < "/proc/$pid/cmdline" | grep -m1 '^:[0-9]' || true)"
      [ -n "$d" ] && export DISPLAY="$d"
    fi
  fi
}

stage_prep() {
  "$SPK/cpp/build.sh" >/dev/null
  "$SPK/rust/build.sh" >/dev/null
  mkdir -p "$HERE/build"
  export PKG_CONFIG_PATH="$ROOT/sys/pkgconfig"
  # shellcheck disable=SC2046
  g++ -std=c++20 -O2 $(pkg-config --cflags alsa) "$HERE/midi_probe.cpp" -o "$PROBE" $(pkg-config --libs alsa)
  echo "prep: $CPP $RUST $PROBE"
}

stage_build() {
  local f="$OUT/build.txt" t0 t1
  {
    echo "# lang kind seconds   (g++ $(g++ -dumpfullversion), $("$HOME/.cargo/bin/rustc" --version | cut -d' ' -f1-2), cargo -j ${JOBS:-8}; nproc $(nproc))"
    for i in 1 2 3; do
      rm -rf "$SPK/cpp/build"
      t0=$(us); "$SPK/cpp/build.sh" >/dev/null; t1=$(us); echo "cpp clean $(secs "$t0" "$t1")"
      (cd "$SPK/rust" && PATH="$HOME/.cargo/bin:$PATH" cargo clean -q)
      t0=$(us); "$SPK/rust/build.sh" >/dev/null; t1=$(us); echo "rust clean $(secs "$t0" "$t1")"
    done
    for i in 1 2 3; do
      touch "$SPK/cpp/spike.cpp"
      t0=$(us); "$SPK/cpp/build.sh" >/dev/null; t1=$(us); echo "cpp incremental $(secs "$t0" "$t1")"
      touch "$SPK/rust/src/main.rs"
      t0=$(us); "$SPK/rust/build.sh" >/dev/null; t1=$(us); echo "rust incremental $(secs "$t0" "$t1")"
    done
    for lang in cpp rust; do
      local b s; b="$(bin_of $lang)"; s="$(mktemp)"
      strip -o "$s" "$b"
      echo "$lang size $(stat -c %s "$b") stripped $(stat -c %s "$s") libs $(readelf -d "$b" | awk '/NEEDED/{gsub(/[][]/,"",$5); printf "%s,", $5}')"
      rm -f "$s"
    done
  } | tee "$f"
}

stage_dsp() {
  mkdir -p "$OUT/dsp"
  for core in 2 20; do
    for i in 1 2 3; do
      for lang in cpp rust; do
        taskset -c "$core" "$(bin_of $lang)" --bench-dsp 20 > "$OUT/dsp/$lang-core$core-$i.json"
      done
    done
  done
  echo "dsp: $(ls "$OUT/dsp" | wc -l) runs"
}

stage_sweep() {
  mkdir -p "$OUT/sweep"
  touch "$OUT/sweep/stderr.txt"
  local rep P F lang
  local cells="${CELLS:-16 2,16 3,16 4,32 2,32 3,32 4,64 2,64 3,64 4,128 2}"
  for rep in $(seq "${FROM_REP:-1}" "${REPS:-2}"); do
    IFS=, read -ra grid <<< "$cells"
    for cfg in "${grid[@]}"; do
      set -- $cfg; P=$1; F=$2
      for lang in cpp rust; do  # interleaved, so drift in machine load hits both languages alike
        local j="$OUT/sweep/$lang-p$P-f$F-r$rep.json"
        rm -f "$j"
        timeout -s INT $((SWEEP_S + 20)) "$(bin_of $lang)" --device "$DEV" --period "$P" --periods "$F" \
          --duration "$SWEEP_S" --no-window --rt --json "$j" >/dev/null 2>>"$OUT/sweep/stderr.txt" || true
        if [ ! -s "$j" ]; then
          echo "{\"lang\":\"$lang\",\"period\":$P,\"fill_periods\":$F,\"failed\":true}" > "$j"
        fi
        echo "sweep r$rep $lang P=$P fill=$F: $(head -c 400 "$j" | grep -oE '"xruns_play":[0-9]+,"xruns_capture":[0-9]+' || echo failed)"
      done
    done
  done
}

stage_soak() {
  local P="${1:?period}" F="${2:?fill}"
  adopt_session
  mkdir -p "$OUT/soak"
  for lang in cpp rust; do
    local d="$OUT/soak/$lang-p$P-f$F"
    "$(bin_of $lang)" --device "$DEV" --period "$P" --periods "$F" --duration "$SOAK_S" --rt \
      --json "$d.json" --notes "$d-notes.csv" >/dev/null 2>"$d-stderr.txt" &
    local spid=$!
    "$PROBE" --to "op1og-spike-$lang" --seconds $((SOAK_S - 15)) --csv "$d-probe.csv" 2>>"$d-stderr.txt" &
    local ppid=$!
    wait "$spid" || true
    wait "$ppid" || true
    echo "soak $lang: $(head -c 600 "$d.json")"
  done
}

stage_pipewire() {
  mkdir -p "$OUT/pipewire"
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  for cfg in "64 2" "64 3" "128 2"; do
    set -- $cfg
    for lang in cpp rust; do
      local d="$OUT/pipewire/$lang-p$1-f$2"
      timeout -s INT $((PW_S + 30)) "$(bin_of $lang)" --device pipewire --period "$1" --periods "$2" --duration "$PW_S" \
        --no-capture --mute --no-window --rt --json "$d.json" --notes "$d-notes.csv" >/dev/null 2>"$d-stderr.txt" &
      local spid=$!
      "$PROBE" --to "op1og-spike-$lang" --seconds $((PW_S - 10)) --csv "$d-probe.csv" 2>>"$d-stderr.txt" &
      local ppid=$!
      wait "$spid" || true
      wait "$ppid" || true
      echo "pipewire $lang P=$1 fill=$2: $(head -c 400 "$d.json")"
    done
  done
}

stage_shots() {
  adopt_session
  mkdir -p "$SHARED/screenshots"
  for lang in cpp rust; do
    local bmp; bmp="$(mktemp --suffix=.bmp)"
    timeout -s INT 30 "$(bin_of $lang)" --device "$DEV" --period 32 --periods 4 --duration 8 --rt --shot "$bmp" >/dev/null 2>&1 || true
    python3 -I "$HERE/bmp2png.py" "$bmp" "$SHARED/screenshots/t3-spike-$lang.png"
    rm -f "$bmp"
  done
}

case "${1:-}" in
  prep) stage_prep ;;
  build) stage_build ;;
  dsp) stage_dsp ;;
  sweep) stage_sweep ;;
  soak) shift; stage_soak "$@" ;;
  pipewire) stage_pipewire ;;
  shots) stage_shots ;;
  *) sed -n '2,16p' "$0"; exit 64 ;;
esac
