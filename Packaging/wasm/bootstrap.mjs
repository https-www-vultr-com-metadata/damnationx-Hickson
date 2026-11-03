import { browserAssets } from './local-assets.mjs';

const $ = id => document.getElementById(id);
const status = message => { $('status').textContent = message; };
let busy = false;
let started = false;
function fail(error) {
  console.error(error);
  document.body.classList.remove('playing');
  $('error').textContent = error?.message || String(error);
  status('The game could not start.');
  // A WASM instance may be partially initialized; retry with a fresh page.
  $('play').textContent = started ? 'Reload to retry' : 'Play';
  $('play').disabled = false;
}
async function refresh() {
  const summary = browserAssets.summary();
  status(summary.game ? 'Your game files are ready. Optional D2R and HD assets are stored locally.' : 'Choose your Diablo game files to begin.');
  $('play').disabled = busy || !summary.game;
}
async function importing(action) {
  busy = true;
  $('error').textContent = '';
  for (const input of document.querySelectorAll('input,select,button')) input.disabled = true;
  try { await action(message => status(typeof message === 'string' ? message : JSON.stringify(message))); }
  catch (error) { $('error').textContent = error.message || String(error); }
  finally {
    busy = false;
    for (const input of document.querySelectorAll('input,select,button')) input.disabled = false;
    await refresh();
  }
}
$('game-files').onchange = event => importing(progress => browserAssets.importGameFiles(event.target.files, progress));
$('d2r-files').onchange = event => importing(progress => browserAssets.importFolder(event.target.files, 'd2r', progress));
function importRawD2r(selectedFiles) {
  const categories = [...document.querySelectorAll('[data-d2r-category]:checked')].map(input => input.dataset.d2rCategory);
  const textureSize = Number($('d2r-texture-size').value);
  importing(async progress => {
    if (!categories.length) throw new Error('Choose at least one D2R asset category.');
    const controller = new AbortController();
    $('d2r-cancel').hidden = false;
    $('d2r-cancel').disabled = false;
    $('d2r-cancel').onclick = () => controller.abort();
    try {
      const {convertD2rInstall} = await import('./d2r-converter.mjs');
      const files = await selectedFiles;
      await convertD2rInstall(files, {categories, textureSize, signal: controller.signal}, progress);
    } finally { $('d2r-cancel').hidden = true; }
  });
}
$('d2r-raw-files').onchange = event => importRawD2r(event.target.files);
if (window.showDirectoryPicker) {
  $('d2r-pick-install').hidden = false;
  $('d2r-raw-files').hidden = true;
  $('d2r-pick-install').onclick = () => {
    const choosing = window.showDirectoryPicker({mode: 'read'});
    importRawD2r(choosing.then(async handle => {
      const {filesFromD2rDirectory} = await import('./d2r-converter.mjs');
      return filesFromD2rDirectory(handle);
    }));
  };
}
$('hd-files').onchange = event => importing(progress => browserAssets.importFolder(event.target.files, 'hd', progress));
$('play').onclick = async () => {
  if (started) { location.reload(); return; }
  busy = true;
  started = true;
  $('play').disabled = true;
  $('error').textContent = '';
  try {
    if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') throw new Error('Open the 3D launcher directly in a current browser. This page cannot start the game in the present context.');
    if (!isSecureContext || !navigator.gpu) throw new Error('This browser needs WebGPU on HTTPS. Use a browser with WebGPU enabled.');
    let adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
      adapter = await navigator.gpu.requestAdapter({forceFallbackAdapter: true});
      globalThis.damnationxForceFallbackAdapter = !!adapter;
    }
    if (!adapter) throw new Error('No WebGPU adapter is available. Check hardware acceleration in your browser.');
    status('Loading the game simulation…');
    const { default: createCore } = await import('./damnationx_core.js');
    const core = await createCore({
      locateFile: name => new URL(name, import.meta.url).href,
      print: line => console.info('[core]', line),
      printErr: line => console.warn('[core]', line),
      onAbort: reason => fail(new Error(`Game simulation stopped: ${reason}`)),
    });
    // The simulation worker may grow shared memory between frame handoffs.
    // Keep the public heap view fresh for snapshot copies into the renderer.
    let exportedHeap = core.HEAPU8;
    Object.defineProperty(core, 'HEAPU8', {
      configurable: true,
      get() { core._dx_host_refresh_heap(); return exportedHeap; },
      set(value) { exportedHeap = value; },
    });
    globalThis.damnationxCore = core;
    await browserAssets.mountCore(core);
    core.FS.mkdirTree('/preferences');
    core.FS.mount(core.IDBFS, {}, '/preferences');
    await new Promise((resolve, reject) => core.FS.syncfs(true, error => error ? reject(error) : resolve()));
    // Keep saves durable while playing; pagehide alone cannot finish async IDB writes.
    let syncing = false;
    const persist = () => {
      if (syncing) return;
      syncing = true;
      core.FS.syncfs(false, error => { syncing = false; if (error) console.warn('Save persistence:', error); });
    };
    setInterval(persist, 5000);
    addEventListener('pagehide', persist);
    const summary = browserAssets.summary();
    globalThis.damnationxArgs = ['--render-mode', $('render-mode').value, '--rt', 'off'];
    if (summary.d2r) globalThis.damnationxArgs.push('--d2r-dir', '/d2r', '--d2r-models', 'on', '--d2r-env', 'on');
    if (summary.hd) globalThis.damnationxArgs.push('--hd-dir', '/hd');
    globalThis.damnationxArgs.push('--', '--data-dir', '/data', '--config-dir', '/preferences', '--save-dir', '/preferences');
    status('Loading the 3D renderer…');
    const { default: init } = await import('./host/damnationx_host.js');
    document.body.classList.add('playing');
    $('canvas').focus();
    const {installBrowserPointer} = await import('./browser-pointer.mjs');
    installBrowserPointer($('canvas'));
    const {installWebXr} = await import('./webxr.mjs');
    installWebXr({canvas: $('canvas')});
    await init();
  } catch (error) { fail(error); }
};
addEventListener('error', event => { if (started) fail(event.error || new Error(event.message)); });
addEventListener('unhandledrejection', event => { if (started) fail(event.reason); });
try { await browserAssets.restore(); await refresh(); }
catch (error) { fail(error); }
