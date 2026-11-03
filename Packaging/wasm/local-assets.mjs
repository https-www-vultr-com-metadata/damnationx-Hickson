// Player-owned files remain in this browser. Nothing in this module uploads assets.
const files = new Map();
const syncFiles = new Map();
const decoder = new TextDecoder();
const normalize = path => {
  const parts = String(path).replaceAll('\\', '/').split('/').filter(Boolean);
  if (parts.some(part => part === '..' || part === '.')) throw new Error('Invalid local asset path');
  return '/' + parts.join('/');
};
let database;
function db() {
  return database ??= new Promise((resolve, reject) => {
    const request = indexedDB.open('damnationx-local-assets', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('files');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}
async function store(entries, prefix, signal) {
  if (signal?.aborted) throw new Error('D2R conversion cancelled');
  const connection = await db();
  if (signal?.aborted) throw new Error('D2R conversion cancelled');
  return new Promise((resolve, reject) => {
    const transaction = connection.transaction('files', 'readwrite');
    const cancel = () => { try { transaction.abort(); } catch {} };
    signal?.addEventListener('abort', cancel, {once: true});
    const cleanup = () => signal?.removeEventListener('abort', cancel);
    const objectStore = transaction.objectStore('files');
    if (prefix) {
      const cursor = objectStore.openKeyCursor();
      cursor.onsuccess = () => {
        const item = cursor.result;
        if (!item) { for (const [path, blob] of entries) objectStore.put(blob, path); return; }
        if (String(item.key).startsWith(prefix + '/')) objectStore.delete(item.key);
        item.continue();
      };
    } else for (const [path, blob] of entries) objectStore.put(blob, path);
    transaction.oncomplete = () => { cleanup(); resolve(); };
    transaction.onabort = () => { cleanup(); reject(transaction.error || new Error(signal?.aborted ? 'D2R conversion cancelled' : 'Local storage transaction aborted')); };
    transaction.onerror = () => { cleanup(); reject(transaction.error); };
  });
}
// Manifest/config readers and facade geometry are synchronous in the shared host.
// Character meshes, HUD images and audio stay as Blobs and decode on demand.
// Environment terrain and optional HD2D files retain bytes for synchronous presenters.
const needsSync = path => /\.(json|toml|ron|ini|txt|tsv)$/i.test(path) || /^\/d2r\/env\//i.test(path) || /^\/hd\//i.test(path);
async function prepare(entries, signal) {
  const sync = new Map();
  for (const [path, blob] of entries) {
    if (signal?.aborted) throw new Error('D2R conversion cancelled');
    if (needsSync(path)) sync.set(path, new Uint8Array(await blob.arrayBuffer()));
  }
  return sync;
}
function coreRead(path) {
  const fs = globalThis.damnationxCore?.FS;
  if (!fs) throw new Error('Local file missing: ' + path);
  return fs.readFile(path);
}
export const browserAssets = {
  async restore() {
    const connection = await db();
    const entries = await new Promise((resolve, reject) => {
      const result = [];
      const transaction = connection.transaction('files', 'readonly');
      const request = transaction.objectStore('files').openCursor();
      request.onsuccess = () => {
        const item = request.result;
        if (!item) { resolve(result); return; }
        result.push([item.key, item.value]); item.continue();
      };
      request.onerror = () => reject(request.error);
    });
    const prepared = await prepare(entries);
    for (const [path, blob] of entries) files.set(path, blob);
    for (const [path, bytes] of prepared) syncFiles.set(path, bytes);
    return this.summary();
  },
  async importFolder(fileList, type = 'd2r', progress = () => {}, signal) {
    if (!['d2r', 'hd'].includes(type)) throw new Error('Unknown asset folder type');
    const prefix = type === 'd2r' ? '/d2r' : '/hd';
    const entries = new Map();
    for (const file of fileList) {
      const relative = file.webkitRelativePath ? file.webkitRelativePath.split('/').slice(1).join('/') : file.name;
      const path = normalize(prefix + '/' + relative);
      if (entries.has(path)) throw new Error('Duplicate file: ' + relative);
      entries.set(path, file);
    }
    if (!entries.size) throw new Error('No files selected');
    if (type === 'd2r') {
      const mapping = entries.get('/d2r/mapping.resolved.json');
      const environment = entries.get('/d2r/env/env.resolved.json');
      if (!mapping && !environment) throw new Error('Choose the converted glb folder containing mapping.resolved.json or env/env.resolved.json. The raw D2R install must first be converted with tools/d2r-import.');
      for (const manifest of [mapping, environment].filter(Boolean)) JSON.parse(await manifest.text());
      // Reject broken mapping references before replacing a working import.
      if (mapping) {
        const data = JSON.parse(await mapping.text());
        for (const entry of data.entries || []) {
          for (const model of [entry, entry.model, ...(entry.options || [])].filter(Boolean)) {
            if (typeof model.glb === 'string' && !entries.has(normalize(prefix + '/' + model.glb))) throw new Error('Model missing from selected folder: ' + model.glb);
          }
        }
      }
    }
    if (type === 'd2r' && entries.has('/d2r/env/env.resolved.json')) {
      const environment = JSON.parse(await entries.get('/d2r/env/env.resolved.json').text());
      const references = [];
      for (const model of Object.values(environment.models || {})) {
        if (model.glb) references.push(model.glb);
        for (const lod of model.contract?.lods || []) if (lod.glb) references.push(lod.glb);
      }
      for (const material of Object.values(environment.materials || {})) {
        for (const field of ['albedo', 'normal', 'orm', 'height']) if (material[field]) references.push(material[field]);
      }
      for (const relative of references) {
        if (typeof relative !== 'string' || !entries.has(normalize('/d2r/' + relative))) throw new Error('Environment asset missing from selected folder: ' + relative);
      }
    }
    progress('Preparing local metadata…');
    const prepared = await prepare(entries, signal);
    progress('Saving owned files in this browser…');
    await store(entries, prefix, signal); // Commit before replacing the active import.
    for (const path of files.keys()) if (path.startsWith(prefix + '/')) files.delete(path);
    for (const path of syncFiles.keys()) if (path.startsWith(prefix + '/')) syncFiles.delete(path);
    for (const [path, blob] of entries) files.set(path, blob);
    for (const [path, bytes] of prepared) syncFiles.set(path, bytes);
    await navigator.storage?.persist?.();
    return this.summary();
  },
  async importGameFiles(fileList, progress = () => {}) {
    const entries = new Map();
    for (const file of fileList) {
      if (!/\.mpq$/i.test(file.name)) continue;
      const path = normalize('/data/' + file.name);
      if (file.name.includes('/') || file.name.includes('\\')) throw new Error('Invalid archive filename');
      const magic = new Uint8Array(await file.slice(0, 4).arrayBuffer());
      if (magic[0] !== 77 || magic[1] !== 80 || magic[2] !== 81 || ![26, 27].includes(magic[3])) throw new Error('Invalid MPQ archive: ' + file.name);
      if ([...entries.keys()].some(key => key.toLowerCase() === path.toLowerCase())) throw new Error('Duplicate archive: ' + file.name);
      entries.set(path, file);
    }
    if (![...entries.keys()].some(path => /\/(diabdat|spawn)\.mpq$/i.test(path))) throw new Error('Choose DIABDAT.MPQ from your Diablo installation (or spawn.mpq for the shareware game).');
    progress('Saving game archives in this browser…');
    await store(entries, '/data');
    for (const path of files.keys()) if (path.startsWith('/data/')) files.delete(path);
    for (const [path, blob] of entries) files.set(path, blob);
    await navigator.storage?.persist?.();
    return this.summary();
  },
  async mountCore(core) {
    globalThis.damnationxCore = core;
    const fs = core.FS;
    if (!fs) throw new Error('The game core does not expose its local filesystem');
    for (const directory of ['/data', '/hd', '/preferences']) fs.mkdirTree(directory);
    for (const [path, blob] of files) {
      if (!/^\/(data|hd|preferences)\//.test(path)) continue;
      fs.mkdirTree(path.slice(0, path.lastIndexOf('/')));
      fs.writeFile(path, syncFiles.get(path) || new Uint8Array(await blob.arrayBuffer()));
    }
  },
  async syncCore(core = globalThis.damnationxCore) {
    const fs = core?.FS;
    if (!fs) return;
    const entries = [];
    const walk = directory => {
      for (const name of fs.readdir(directory)) {
        if (name === '.' || name === '..') continue;
        const path = directory + '/' + name;
        const stat = fs.stat(path);
        if (fs.isDir(stat.mode)) { walk(path); continue; }
        const bytes = fs.readFile(path);
        const previous = syncFiles.get(path);
        if (previous && previous.length === bytes.length && previous.every((value, index) => value === bytes[index])) continue;
        const copy = new Uint8Array(bytes);
        const blob = new Blob([copy]);
        syncFiles.set(path, copy); files.set(path, blob); entries.push([path, blob]);
      }
    };
    try { walk('/preferences'); } catch (error) { console.warn('Could not inspect browser saves:', error); }
    if (entries.length) await store(entries);
  },
  async read(path) {
    path = normalize(path);
    const bytes = syncFiles.get(path);
    if (bytes) return bytes;
    const file = files.get(path);
    return file ? new Uint8Array(await file.arrayBuffer()) : coreRead(path);
  },
  readSync(path) {
    path = normalize(path);
    const bytes = syncFiles.get(path);
    if (bytes) return bytes;
    return coreRead(path);
  },
  kind(path) {
    path = normalize(path);
    if (files.has(path) || syncFiles.has(path)) return 1;
    for (const name of files.keys()) if (name.startsWith(path === '/' ? '/' : path + '/')) return 2;
    try {
      const fs = globalThis.damnationxCore?.FS;
      const stat = fs.stat(path);
      return fs.isDir(stat.mode) ? 2 : 1;
    } catch { return 0; }
  },
  write(path, data) {
    path = normalize(path);
    const bytes = new Uint8Array(data);
    syncFiles.set(path, bytes);
    const blob = new Blob([bytes]); files.set(path, blob);
    const fs = globalThis.damnationxCore?.FS;
    if (fs && path.startsWith('/preferences/')) {
      fs.mkdirTree(path.slice(0, path.lastIndexOf('/')));
      fs.writeFile(path, bytes);
    }
    store([[path, blob]]).catch(error => console.error('Could not save browser preferences:', error));
  },
  summary() {
    let bytes = 0;
    for (const file of files.values()) bytes += file.size;
    return { files: files.size, bytes, d2r: files.has('/d2r/mapping.resolved.json') || files.has('/d2r/env/env.resolved.json'), hd: this.kind('/hd') === 2, game: [...files.keys()].some(path => /^\/data\/(diabdat|spawn)\.mpq$/i.test(path)) };
  },
  text(path) { return decoder.decode(this.readSync(path)); },
};
globalThis.DamnationXBrowserAssets = browserAssets;
