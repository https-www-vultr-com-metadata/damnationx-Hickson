import {test} from 'node:test';
import assert from 'node:assert/strict';
import {browserAssets} from './local-assets.mjs';
function file(path, text) {
  const blob = new Blob([text]);
  blob.name = path.split('/').at(-1);
  blob.webkitRelativePath = 'glb/' + path;
  return blob;
}
test('raw installations are rejected with conversion instructions', async () => {
  await assert.rejects(browserAssets.importFolder([file('.build.info', 'owned D2R')]), /converted glb folder/);
});
test('broken mapping cannot replace a working local import', async () => {
  const mapping = JSON.stringify({entries:[{glb:'missing.glb'}]});
  await assert.rejects(browserAssets.importFolder([file('mapping.resolved.json',mapping)]), /Model missing/);
  assert.equal(browserAssets.summary().files, 0);
});
test('selected asset paths cannot escape their local root', async () => {
  await assert.rejects(browserAssets.importFolder([file('../outside.json','{}')]), /Invalid local asset path/);
  assert.throws(() => browserAssets.kind('/d2r/../outside'), /Invalid local asset path/);
});
test('duplicate entries and malformed JSON are rejected before persistence', async () => {
  await assert.rejects(browserAssets.importFolder([file('mapping.resolved.json','{}'),file('mapping.resolved.json','{}')]), /Duplicate file/);
  await assert.rejects(browserAssets.importFolder([file('mapping.resolved.json','invalid')]), SyntaxError);
});
test('the absolute asset source can read core MEMFS without a network request', async () => {
  globalThis.damnationxCore = {FS:{readFile(path){ assert.equal(path,'/assets/test.bin');return new Uint8Array([12,34]);},stat(){return {mode:1};},isDir(){return false;}}};
  assert.deepEqual(await browserAssets.read('/assets/test.bin'),new Uint8Array([12,34]));
  assert.equal(browserAssets.kind('/assets/test.bin'),1);
  delete globalThis.damnationxCore;
});
