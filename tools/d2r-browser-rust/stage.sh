#!/bin/sh
# Build a Python-free browser converter into a clean staging directory.
set -eu
if [ "$#" -ne 2 ]; then printf '%s\n' 'Usage: stage.sh OUTPUT_DIR BUILD_DIR' >&2; exit 2; fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$1" "$2"
out=$(CDPATH= cd -- "$1" && pwd)
build=$(CDPATH= cd -- "$2" && pwd)
# Remove artifacts from the retired browser runtime when reusing an output.
rm -rf "$out/pyodide"
rm -f "$out/d2r-browser.py" "$out/d2r-import.zip"
"$here/build.sh" "$out" "$build/rust"
if [ -n "${D2R_CASC_SOURCE:-}" ]; then
  emcmake cmake -S "$here/../d2r-import/browser" -B "$build/casc" -G Ninja -DCMAKE_BUILD_TYPE=Release -DD2R_BROWSER_NODE_TEST=OFF "-DFETCHCONTENT_SOURCE_DIR_CASCLIB=$D2R_CASC_SOURCE"
else
  emcmake cmake -S "$here/../d2r-import/browser" -B "$build/casc" -G Ninja -DCMAKE_BUILD_TYPE=Release -DD2R_BROWSER_NODE_TEST=OFF
fi
cmake --build "$build/casc" --target d2r_browser_tool -j "${D2R_BUILD_JOBS:-4}"
cp "$build/casc/d2r-casc.mjs" "$build/casc/d2r-casc.wasm" "$out/"
cp "$here/../../Packaging/wasm/d2r-converter.mjs" "$out/"
cp "$out/d2r-rust-worker.mjs" "$out/d2r-converter-worker.mjs"
mkdir -p "$out/d2r-converter-licenses"
cp "$here/../d2r-import/native/third_party/bitknit2.h" "$out/d2r-converter-licenses/BitKnit2.h"
for source in "${D2R_CASC_SOURCE:-}" "$build/casc/_deps/casclib-src" "$build/casc/native/_deps/casclib-src"; do
  if [ -f "$source/LICENSE" ]; then
    cp "$source/LICENSE" "$out/d2r-converter-licenses/CascLib-LICENSE.txt"
    cp "$source/src/zlib/zlib.h" "$out/d2r-converter-licenses/CascLib-zlib.h"
    break
  fi
done
printf 'Python-free browser converter staged at %s\n' "$out"
