#!/bin/sh
# Build the WebAssembly converter, workers and recipes for staging.
set -eu
if [ "$#" -ne 2 ]; then
  printf '%s\n' 'Usage: build.sh OUTPUT_DIR BUILD_DIR' >&2
  exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$1" "$2"
out=$(CDPATH= cd -- "$1" && pwd)
build=$(CDPATH= cd -- "$2" && pwd)
cargo build --manifest-path "$here/Cargo.toml" --target wasm32-unknown-unknown --release --locked --target-dir "$build"
wasm-bindgen --target web --out-dir "$out" "$build/wasm32-unknown-unknown/release/d2r_browser_rust.wasm"
cp "$here/casc-bridge.mjs" "$out/casc-bridge.mjs"
cp "$here/d2r-rust-worker.mjs" "$out/d2r-rust-worker.mjs"
cp "$here/d2r-rust-unit-worker.mjs" "$out/d2r-rust-unit-worker.mjs"
cp "$here/../d2r-import/mapping.toml" "$out/mapping.toml"
cp "$here/../d2r-import/env_mapping.toml" "$out/env_mapping.toml"
printf 'Rust browser primitives staged at %s\n' "$out"
