import assert from 'node:assert/strict';
import { createCascBridge } from './casc-bridge.mjs';
const files = new Map();
const calls = [];
let failFlatten = false;
const casc = {
  ccall(name, _result, _types, args) {
    calls.push([name, ...args]);
    if (name === 'd2rb_list') files.set(args[0], '2\tdata:asset\\one\n');
    if (name === 'd2rb_extract') files.set(args[1], new Uint8Array([1, 2]));
    if (name === 'd2rb_flatten') { if (failFlatten) return 3; files.set(args[1], new Uint8Array([3, 4])); }
    return 0;
  },
  FS: {
    readFile(path) { if (!files.has(path)) throw Error('missing'); return files.get(path); },
    unlink(path) { files.delete(path); },
  },
};
const bridge = createCascBridge(casc);
assert.throws(() => bridge.listing(), /not open/);
bridge.open();
assert.match(bridge.listing(), /asset/);
assert.deepEqual(bridge.read('asset/one', true), new Uint8Array([3, 4]));
assert.equal(files.size, 0);
assert.ok(calls.some(c => c[0] === 'd2rb_extract' && c[1] === 'data:asset\\one'));
assert.throws(() => bridge.read('../evil'), /Invalid/);
failFlatten = true;
assert.throws(() => bridge.read('asset/one', true), /decompress/);
assert.equal(files.size, 0);
bridge.close();
assert.throws(() => bridge.read('asset/one'), /not open/);
console.log('CASC bridge tests passed');

// Battle.net installs open by folder; Steam's static storage by Data/.build.config.
import { ownedBuildName, ownedStoragePath } from './casc-bridge.mjs';
assert.equal(ownedStoragePath(['.build.info', 'Data/data/0000000001.idx']), '/owned');
assert.equal(ownedStoragePath(['D2R.exe', 'Data/.build.config', 'Data/00-00000001.data']), '/owned/Data/.build.config');
assert.equal(ownedStoragePath(['data/.build.config'], '/src'), '/src/data/.build.config');
assert.equal(ownedStoragePath(['Data/00-00000001.data']), '/owned');
const encode = text => new TextEncoder().encode(text);
assert.equal(ownedBuildName(encode('﻿Branch!STRING:0|Version!STRING:0\nus|1.6.80273\n'), false), '1.6.80273');
assert.equal(ownedBuildName(encode('Branch!STRING:0\nus\n'), false), '?');
assert.equal(ownedBuildName(encode('build-name = D2R-3.2.0.1_93236\n'), true), 'D2R-3.2.0.1_93236');
const steamConfig = encode('# Build Configuration\nroot = bf9064519cc56ba5fe197ce27e4729a1\n');
const steamName = ownedBuildName(steamConfig, true);
assert.match(steamName, /^steam-[0-9a-f]{16}$/);
assert.equal(ownedBuildName(steamConfig, true), steamName);
assert.notEqual(ownedBuildName(encode('root = 00\n'), true), steamName);
assert.match(ownedBuildName(encode('build-name = ../x\n'), true), /^steam-/);
// Same FNV-1a 64 as the native converter, so both name a Steam build alike.
assert.equal(ownedBuildName(encode(''), true), 'steam-cbf29ce484222325');
console.log('Owned storage path tests passed');
