# Rust D2R browser importer

The browser converter uses the shared Rust conversion crate compiled to
WebAssembly and the existing local CASC/Granny helper compiled with Emscripten.
The worker reads the selected owned installation through WORKERFS, runs model
conversions in short-lived child workers to release WebAssembly memory, and
streams files to the launcher. The launcher validates and commits the complete
import in one IndexedDB transaction. No installation or converted game assets
are uploaded.

`stage.sh OUTPUT_DIR BUILD_DIR` builds the importer and its archive helper into
an output directory. It requires Rust's `wasm32-unknown-unknown` target,
`wasm-bindgen-cli` 0.2.129, CMake, Ninja, and Emscripten. `build.sh` stages just
the Rust module and recipes for development.

To build the complete browser host and stage this importer, use
`tools/build-wasm-host.sh`. Run `node tools/d2r-browser-rust/atomic.test.mjs` and
`node tools/d2r-browser-rust/casc-bridge.test.mjs` for coordinator and bridge
regressions. `owned-browser-smoke.mjs` exercises real Chromium WORKERFS with a
locally owned installation; its `worker-full` mode tests every category and
validates and commits the result to IndexedDB. It serves range requests only on
loopback and deletes its temporary browser profile after the test.
