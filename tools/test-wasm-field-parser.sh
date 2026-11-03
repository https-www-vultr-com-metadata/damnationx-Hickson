#!/usr/bin/env bash
# Real Emscripten/fiber regression; requires em++ and Node, no game assets.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/damnationx-field-parser.XXXXXX")
trap 'rm -rf "$probe_dir"' EXIT
"${EMXX:-em++}" "$root/test/host_browser_field_parser.cpp" "$root/Source/data/parser.cpp" \
  -I "$root/Source" -std=c++23 -O3 -fno-exceptions \
  -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=4194304 -sSTACK_SIZE=8MB \
  -sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 \
  -o "$probe_dir/field-parser.js"
"${NODE:-node}" "$probe_dir/field-parser.js"
