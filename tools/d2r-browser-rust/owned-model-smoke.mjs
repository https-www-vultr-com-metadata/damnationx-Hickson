// Node-only integration smoke. Uses the same Rust WASM and CASC bridge as the
// browser, with NODEFS standing in for WORKERFS. Pass an owned D2R install.
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { createCascBridge } from './casc-bridge.mjs';
import path from 'node:path';
const [cascModule, wasmModule, game, unit = 'data/hd/character/enemy/zombie1.json'] = process.argv.slice(2);
if (!cascModule || !wasmModule || !game) throw Error('usage: node owned-model-smoke.mjs CASC_MJS WASM_JS OWNED_D2R_DIR [UNIT_JSON]');
const {default: create} = await import(pathToFileURL(path.resolve(cascModule)));
const wasm = createRequire(import.meta.url)(path.resolve(wasmModule));
const casc = await create({noInitialRun:true});
casc.FS.mkdirTree('/owned'); casc.FS.mkdirTree('/scratch');
casc.FS.mount(casc.NODEFS, {root: game}, '/owned');
const bridge = createCascBridge(casc);
bridge.open();
try {
  const model = wasm.convert_model(bridge, unit, '{}', false);
  try {
    const glb = model.glb;
    const meta = JSON.parse(model.metadata);
    if (new TextDecoder().decode(glb.slice(0, 4)) !== 'glTF') throw Error('Invalid GLB magic');
    if (!meta.meshes || !meta.bones) throw Error('Model has no meshes or bones');
    console.log(JSON.stringify({unit, bytes: glb.length, ...meta}));
  } finally { model.free(); }
} finally { bridge.close(); }
