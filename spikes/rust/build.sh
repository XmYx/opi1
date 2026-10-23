#!/bin/sh
# Build the Rust spike: spikes/rust/target/release/spike-rust. Headers/link names come from sys/setup.sh.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
"$ROOT/sys/setup.sh" >/dev/null
export PKG_CONFIG_PATH="$ROOT/sys/pkgconfig"
export PATH="$HOME/.cargo/bin:$PATH"
cd "$HERE"
cargo build --release --quiet -j "${JOBS:-8}"
echo "$HERE/target/release/spike-rust"
