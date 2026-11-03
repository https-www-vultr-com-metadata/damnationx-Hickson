// Synchronous adapter around the existing Emscripten CASC/Granny helper.
// It keeps archive access separate from the shared Rust conversion primitives.

// The path CascLib opens for an install mounted at `root`, given the mounted file
// paths relative to it. Battle.net is opened by its folder. Steam (no .build.info)
// is opened by its Data/.build.config: given a folder, CascLib only looks for a
// lowercase data/.build.config, and the case-sensitive mount keeps the name Data.
export function ownedStoragePath(paths, root = '/owned') {
  if (paths.includes('.build.info')) return root;
  const config = paths.find(path => /^data\/\.build\.config$/i.test(path));
  return config ? `${root}/${config}` : root;
}

// The build name written to manifests: Battle.net's Version column, or Steam's
// build-name, else a hash of Steam's build config (as the native converter names it).
export function ownedBuildName(bytes, steam) {
  const text = new TextDecoder().decode(bytes).replace(/^﻿/, '');
  const lines = text.split(/\r?\n/);
  if (!steam) {
    const index = (lines[0] || '').split('|').findIndex(h => h.split('!')[0] === 'Version');
    return index < 0 ? '?' : ((lines[1] || '').split('|')[index] || '?');
  }
  const line = lines.find(l => l.includes('=') && l.slice(0, l.indexOf('=')).trim() === 'build-name');
  const name = line?.slice(line.indexOf('=') + 1).trim();
  if (name && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)) return name;
  let hash = 0xcbf29ce484222325n;
  for (const byte of bytes) hash = ((hash ^ BigInt(byte)) * 0x100000001b3n) & 0xffffffffffffffffn;
  return 'steam-' + hash.toString(16).padStart(16, '0');
}

export function createCascBridge(casc) {
  const call = (name, types, values) => casc.ccall(name, 'number', types, values);
  let opened = false;
  return {
    open(path = '/owned') {
      if (opened) throw new Error('CASC storage is already open');
      const code = call('d2rb_open', ['string'], [path]);
      if (code !== 0) throw new Error(`Cannot open this D2R installation (CASC ${code}). Select the install folder: Battle.net's contains .build.info and Data, Steam's contains Data/.build.config.`);
      opened = true;
    },
    close() {
      if (opened) call('d2rb_close', [], []);
      opened = false;
    },
    listing() {
      if (!opened) throw new Error('CASC storage is not open');
      const output = '/scratch/list.tsv';
      try {
        if (call('d2rb_list', ['string'], [output]) !== 0) throw new Error('Cannot enumerate D2R archive files');
        return casc.FS.readFile(output, { encoding: 'utf8' });
      } finally { try { casc.FS.unlink(output); } catch {} }
    },
    read(path, flatten = false) {
      if (!opened) throw new Error('CASC storage is not open');
      if (typeof path !== 'string' || !path || path.includes('\0') || path.split(/[\\/]/).some(p => p === '.' || p === '..')) throw new Error('Invalid D2R source path');
      const source = '/scratch/source', flat = '/scratch/flat';
      try {
        if (call('d2rb_extract', ['string', 'string'], ['data:' + path.replaceAll('/', '\\'), source]) !== 0) throw new Error('D2R file not found: ' + path);
        if (flatten && call('d2rb_flatten', ['string', 'string'], [source, flat]) !== 0) throw new Error('Cannot decompress Granny file: ' + path);
        // Copy out of the Emscripten heap before scratch files are deleted.
        return new Uint8Array(casc.FS.readFile(flatten ? flat : source));
      } finally {
        for (const file of [source, flat]) try { casc.FS.unlink(file); } catch {}
      }
    },
  };
}
