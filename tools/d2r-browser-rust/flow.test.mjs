// Exercise the browser-side archive adapter -> WebAssembly data boundary.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createCascBridge } from './casc-bridge.mjs';
const require = createRequire(import.meta.url);
const wasm = require(process.argv[2]);
const source = new Uint8Array(48);
source.set(new TextEncoder().encode('<DE('));
const header = new DataView(source.buffer);
header.setUint16(4, 31, true); source[6] = 2; source[7] = 1;
header.setUint32(8, 1, true); header.setUint32(12, 1, true);
header.setUint32(36, 4, true); header.setUint32(40, 4, true);
source.set([7, 8, 9, 255], 44);
const files = new Map();
const casc = {
  ccall(name, _, __, args) {
    if (name === 'd2rb_extract') files.set(args[1], source);
    return 0;
  },
  FS: {
    readFile(name) { if (!files.has(name)) throw Error('missing'); return files.get(name); },
    unlink(name) { files.delete(name); },
  },
};
const bridge = createCascBridge(casc);
bridge.open();
const image = wasm.decode_texture(bridge.read('data/hd/test.texture'), 0);
bridge.close();
assert.deepEqual([...image.rgba], [7, 8, 9, 255]);
assert.equal(files.size, 0);
image.free();
console.log('Browser CASC-to-Rust WASM fixture flow passed');
