#!/usr/bin/env bash
# Build and package the browser host and Rust D2R importer without Python.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build-wasm-host-core"; out="$root/build-wasm-host"; jobs="${D2R_BUILD_JOBS:-4}"
skip_build=0; skip_converter=0; wasm=""; dependencies=""
while (($#)); do
  case "$1" in
    --build-dir) build="$2"; shift 2;; --out-dir) out="$2"; shift 2;; --jobs) jobs="$2"; shift 2;;
    --wasm) wasm="$2"; shift 2;; --dependency-sources) dependencies="$2"; shift 2;;
    --skip-build) skip_build=1; shift;; --skip-converter) skip_converter=1; shift;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 2;;
  esac
done
abs_path() { [[ -z "$1" || "$1" == /* ]] && printf "%s" "$1" || printf "%s/%s" "$PWD" "$1"; }
wasm=$(abs_path "$wasm"); dependencies=$(abs_path "$dependencies")
mkdir -p "$build" "$out"
build=$(CDPATH= cd -- "$build" && pwd); out=$(CDPATH= cd -- "$out" && pwd)
cd "$root"
if (( !skip_build )); then
  configure=(emcmake cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAMNATIONX_HOST_LIB=ON -DDEVILUTIONX_WEBGPU=OFF -DBUILD_TESTING=OFF -DDISABLE_LTO=ON -DDEVILUTIONX_LOBBY=OFF -DDEVILUTIONX_D2_SOUND_IMPORT=OFF)
  if [[ -n "$dependencies" ]]; then
    for dir in "$dependencies"/*-src; do
      [[ -d "$dir" ]] || continue
      name=${dir##*/}; name=${name%-src}; name=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
      configure+=("-DFETCHCONTENT_SOURCE_DIR_${name}=$dir")
    done
  fi
  "${configure[@]}"
  cmake --build "$build" --target damnationx_core -j "$jobs"
  cargo build --locked -p damnationx_host --target wasm32-unknown-unknown --release -j "$jobs"
fi
if [[ -z "$wasm" ]]; then
  target=$(cargo metadata --no-deps --format-version=1 | node -e 'let s="";process.stdin.on("data",c=>s+=c).on("end",()=>process.stdout.write(JSON.parse(s).target_directory))')
  wasm="$target/wasm32-unknown-unknown/release/damnationx_host.wasm"
fi
[[ -f "$wasm" ]] || { printf 'Missing compiled host: %s\n' "$wasm" >&2; exit 1; }
for name in damnationx_core.js damnationx_core.wasm damnationx_core.data; do
  [[ -f "$build/$name" ]] || { printf 'Missing simulation artifact: %s\n' "$build/$name" >&2; exit 1; }
done
for file in "$root"/Packaging/wasm/*; do
  case "$file" in *.test.*) ;; *.html|*.mjs|*.css) cp "$file" "$out/";; esac
done
cp "$build/damnationx_core.js" "$build/damnationx_core.wasm" "$build/damnationx_core.data" "$out/"
[[ ! -f "$build/damnationx_core.worker.js" ]] || cp "$build/damnationx_core.worker.js" "$out/"
mkdir -p "$out/host-assets" "$out/host"
cp -R "$root/host/damnationx_host/assets/." "$out/host-assets/"
wasm-bindgen --target web --no-demangle --remove-name-section --remove-producers-section --out-dir "$out/host" "$wasm"
[[ $(wc -c < "$out/host/damnationx_host_bg.wasm") -le 100000000 ]] || { printf '%s\n' 'Host WASM exceeds the 100 MB single-file limit' >&2; exit 1; }
if (( !skip_converter )); then
  if [[ -d "$dependencies/casclib-src" && -n "$dependencies" ]]; then
    D2R_BUILD_JOBS="$jobs" D2R_CASC_SOURCE="$dependencies/casclib-src" "$root/tools/d2r-browser-rust/stage.sh" "$out" "$build/d2r-converter"
  else
    D2R_BUILD_JOBS="$jobs" "$root/tools/d2r-browser-rust/stage.sh" "$out" "$build/d2r-converter"
  fi
fi
# Build outputs are public: refuse accidentally included game and save files.
if find "$out" -type f \( -iname '*.mpq' -o -iname '*.d2s' -o -iname '*.sv' \) -print -quit | grep -q .; then
  printf '%s\n' 'Refusing to package owned game/save data' >&2; exit 1
fi
revision=$(git rev-parse HEAD)
node - "$out" "$revision" "$skip_converter" <<'NODE'
const fs=require('node:fs'), path=require('node:path'), crypto=require('node:crypto');
const [out,revision,skip]=process.argv.slice(2);const files={};
function walk(dir){for(const e of fs.readdirSync(dir,{withFileTypes:true})){const p=path.join(dir,e.name);if(e.isDirectory())walk(p);else if(e.isFile() && p!==path.join(out,'build.json')){const b=fs.readFileSync(p);files[path.relative(out,p).split(path.sep).join('/')]={bytes:b.length,sha256:crypto.createHash('sha256').update(b).digest('hex')};}}}
walk(out);const manifest={renderer:'bevy-webgpu',emscripten:'unknown',simulation:'real-core',revision,raytracing:false,d2rImport:skip==='1'?'local-converted-folder':'local-install-and-converted-folder',files};fs.writeFileSync(path.join(out,'build.json'),JSON.stringify(manifest,null,2)+'\n');
NODE
printf 'Browser runtime ready: %s\n' "$out"
