#!/usr/bin/env python3
"""Loopback-only WASM QA server. Owned data is read from external caches, never packaged.

python3 tools/test-wasm-host.py --build-dir build-wasm-host --port 8788
Open http://127.0.0.1:8788/__owned/harness.html?auto=1 .
Profiles: smoke (small town subset), town (all town art), full (entire cache).
"""
import argparse
import copy
import json
import mimetypes
from pathlib import Path
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit
import re


def collect_models(value):
    result = set()
    if isinstance(value, dict):
        if isinstance(value.get('model'), str):
            result.add(value['model'])
        if isinstance(value.get('models'), list):
            result.update(v for v in value['models'] if isinstance(v, str))
        for child in value.values():
            result.update(collect_models(child))
    elif isinstance(value, list):
        for child in value:
            result.update(collect_models(child))
    return result


def make_catalog(root, profile):
    replacements = {}
    if profile == 'full':
        paths = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    else:
        mapping = json.loads((root / 'mapping.resolved.json').read_text())
        mapping['entries'] = [entry for entry in mapping['entries'] if entry['kind'] in (0, 2)]
        # The QA hero is a Warrior; one explicit Paladin option avoids loading unused classes.
        mapping['entries'] = [entry for entry in mapping['entries'] if entry['kind'] != 0 or entry['type_id'] == 0]
        paths = set()
        for entry in mapping['entries']:
            if entry['kind'] == 0:
                entry['options'] = [option for option in entry.get('options', []) if option.get('glb') == 'hero_paladin.glb']
                entry['glb'] = 'hero_paladin.glb'
            for model in [entry, *entry.get('options', [])]:
                if model.get('glb'):
                    paths.add(model['glb'])
        replacements['mapping.resolved.json'] = json.dumps(mapping).encode()
        environment = copy.deepcopy(json.loads((root / 'env/env.resolved.json').read_text()))
        environment['levels'] = {'town': environment['levels']['town']}
        if profile == 'smoke':
            environment['levels']['town']['place'] = [place for place in environment['levels']['town'].get('place', []) if place['model'] in ('fountain', 'forge', 'cottage3')]
        needed = collect_models(environment['levels'])
        environment['models'] = {key: model for key, model in environment['models'].items() if key in needed}
        for model in environment['models'].values():
            paths.add(model['glb'])
            for lod in model.get('contract', {}).get('lods', []):
                if lod.get('glb'):
                    paths.add(lod['glb'])
        for material in environment['materials'].values():
            for key in ('albedo', 'normal', 'orm', 'height'):
                if isinstance(material.get(key), str):
                    paths.add(material[key])
        replacements['env/env.resolved.json'] = json.dumps(environment).encode()
        for folder in ('ui', 'vfx'):
            paths.update(p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file())
        paths.update(replacements)
    missing = [path for path in paths if path not in replacements and not (root / path).is_file()]
    if missing:
        raise SystemExit(f'Missing converted assets: {missing[:5]}')
    entries = [{'path': path, 'bytes': len(replacements[path]) if path in replacements else (root / path).stat().st_size} for path in sorted(paths)]
    return entries, replacements


HARNESS = '''<!doctype html><meta charset="utf-8"><title>Local WASM QA import</title>
<style>body{background:#151315;color:#eee;font:17px system-ui;max-width:850px;margin:50px auto}pre{white-space:pre-wrap}button{padding:15px}</style>
<h1>Local WASM QA import</h1><p>Player-owned files are read over loopback into this browser. They are never included in the deployment.</p>
<button id="run">Load owned files and run QA hero</button><pre id="status">Ready</pre><pre id="errors"></pre>
<script type="module">
import {browserAssets} from '/local-assets.mjs';
const status=document.getElementById('status'), errors=document.getElementById('errors');
addEventListener('error',event=>errors.textContent+=String(event.error||event.message)+'\\n');
addEventListener('unhandledrejection',event=>errors.textContent+=String(event.reason)+'\\n');
async function run(){
 document.getElementById('run').disabled=true;
 try{
  const catalog=await (await fetch('/__owned/index.json')).json();
  status.textContent=`${catalog.profile}: ${catalog.files.length} converted files, ${(catalog.bytes/1024**2).toFixed(1)} MB; preparing browser storage…`;
  await browserAssets.restore();
  if(!browserAssets.summary().game){
   status.textContent+='\\nLoading owned Diablo archive…';
   const blob=await (await fetch('/__owned/game.mpq')).blob();
   await browserAssets.importGameFiles([new File([blob],'DIABDAT.MPQ')],s=>status.textContent=s);
  }
  if(localStorage.getItem('damnationx-qa-catalog')!==catalog.stamp || !browserAssets.summary().d2r){
   const files=[]; let done=0;
   for(const item of catalog.files){
    const blob=await (await fetch('/__owned/file/'+item.path.split('/').map(encodeURIComponent).join('/'))).blob();
    const file=new File([blob],item.path.split('/').at(-1));
    Object.defineProperty(file,'webkitRelativePath',{value:'glb/'+item.path}); files.push(file);done+=item.bytes;
    status.textContent=`Loading ${files.length}/${catalog.files.length}: ${(done/1024**2).toFixed(1)} MB\\n${item.path}`;
   }
   await browserAssets.importFolder(files,'d2r',s=>status.textContent=s);
   localStorage.setItem('damnationx-qa-catalog',catalog.stamp);
  }
  status.textContent='Local owned files ready. Starting the QA Warrior…';
  location.href='/__owned/play.html';
 }catch(error){errors.textContent=error.stack||String(error);document.getElementById('run').disabled=false;}
}
document.getElementById('run').onclick=run;
if(new URLSearchParams(location.search).get('auto')==='1')run();
</script>'''


RAW_HARNESS = '<!doctype html><meta charset="utf-8"><title>Raw D2R worker smoke</title><style>body{background:#141216;color:#eee;font:17px system-ui;margin:40px}pre{white-space:pre-wrap}</style><h1>Raw D2R browser worker smoke</h1><pre id="status">Loading owned installation index…</pre><script type="module">\nconst status=document.getElementById(\'status\');\nconst worker=new Worker(\'/d2r-converter-worker.mjs\',{type:\'module\'});\nlet count=0, bytes=0, glbs=0;\nworker.onerror=event=>status.textContent+=\'\\nERROR \'+event.message;\nworker.onmessage=({data})=>{\n if(data.type===\'progress\')status.textContent=data.message;\n if(data.type===\'file\'){count++;bytes+=data.bytes.length;if(data.path.endsWith(\'.glb\')){const magic=new TextDecoder().decode(data.bytes.slice(0,4));if(magic!==\'glTF\')throw Error(\'Invalid GLB\');glbs++;}}\n if(data.type===\'error\'){status.textContent=\'FAIL \'+data.message;worker.terminate();}\n if(data.type===\'done\'){status.textContent=\'PASS raw browser WORKERFS + CASC + Granny + Pyodide conversion\\n\'+JSON.stringify({count,bytes,glbs,result:data.result},null,2);worker.terminate();}\n};\nconst index=await (await fetch(\'/__owned/raw-index.json\')).json();\nif(!index.length)throw Error(\'No owned D2R installation configured\');\nconst files=index.map(item=>({path:item.path,size:item.size,url:\'/__owned/raw/\'+item.path.split(\'/\').map(encodeURIComponent).join(\'/\')}));\nworker.postMessage({type:\'convert\',files,options:{categories:[\'monster\'],onlyModels:[\'zombie\'],textureSize:256}});\n</script>'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cache', type=Path, default=Path.home() / 'Library/Application Support/damnationx/d2r-models/glb')
    parser.add_argument('--mpq', type=Path, default=Path('/Volumes/HolyDrive/dev/DamnationX/diabdat.mpq'))
    parser.add_argument('--game', type=Path, help='Owned raw D2R install, for the raw worker QA route')
    parser.add_argument('--port', type=int, default=8788)
    parser.add_argument('--profile', choices=('smoke', 'town', 'full'), default='smoke')
    parser.add_argument('--catalog-only', action='store_true')
    args = parser.parse_args()
    root = args.cache.resolve()
    raw_game = args.game
    if raw_game is None:
        remembered = root.parent / 'game_dir.txt'
        if remembered.is_file():
            raw_game = Path(remembered.read_text().strip())
    raw_game = raw_game.resolve() if raw_game else None
    raw_entries = []
    if raw_game and raw_game.is_dir():
        raw_entries = [{'path': '.build.info', 'size': (raw_game / '.build.info').stat().st_size}]
        raw_entries += [{'path': p.relative_to(raw_game).as_posix(), 'size': p.stat().st_size} for p in (raw_game / 'Data').rglob('*') if p.is_file()]
    raw_allowed = {item['path'] for item in raw_entries}
    entries, replacements = make_catalog(root, args.profile)
    catalog = {'profile': args.profile, 'files': entries, 'bytes': sum(entry['bytes'] for entry in entries)}
    catalog['stamp'] = f"{args.profile}:{catalog['bytes']}:{(root / 'env/env.resolved.json').stat().st_mtime_ns}"
    print(f"D2R profile {args.profile}: {len(entries)} files, {catalog['bytes'] / 1024**2:.1f} MB; game MPQ {args.mpq.stat().st_size / 1024**2:.1f} MB", flush=True)
    if args.catalog_only:
        return
    build = args.build_dir.resolve()
    if not (build / 'index.html').is_file():
        raise SystemExit(f'No browser build at {build}')
    mimetypes.add_type('application/wasm', '.wasm')
    mimetypes.add_type('text/javascript', '.mjs')

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(build), **kw)
        def end_headers(self):
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
            self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
            super().end_headers()
        def data(self, data, content_type):
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        def local_file(self, file):
            size = file.stat().st_size
            start, end = 0, size - 1
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
            if match:
                start = int(match[1])
                end = min(int(match[2]) if match[2] else end, end)
                if start > end or start >= size:
                    return self.send_error(416)
            self.send_response(206 if match else 200)
            self.send_header('Content-Type', mimetypes.guess_type(str(file))[0] or 'application/octet-stream')
            self.send_header('Accept-Ranges', 'bytes')
            if match:
                self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
            self.send_header('Content-Length', str(end - start + 1))
            self.end_headers()
            with file.open('rb') as stream:
                stream.seek(start)
                remaining = end - start + 1
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        def do_GET(self):
            path = unquote(urlsplit(self.path).path)
            if path == '/__owned/raw-index.json':
                return self.data(json.dumps(raw_entries).encode(), 'application/json')
            if path.startswith('/__owned/raw/'):
                relative = path[len('/__owned/raw/'):]
                if not raw_game or relative not in raw_allowed:
                    return self.send_error(404)
                file = (raw_game / relative).resolve()
                if not file.is_relative_to(raw_game):
                    return self.send_error(403)
                return self.local_file(file)
            if path == '/__owned/raw-smoke.html':
                return self.data(RAW_HARNESS.encode(), 'text/html; charset=utf-8')
            if path == '/__owned/harness.html':
                return self.data(HARNESS.encode(), 'text/html; charset=utf-8')
            if path == '/__owned/index.json':
                return self.data(json.dumps(catalog).encode(), 'application/json')
            if path == '/__owned/game.mpq':
                return self.local_file(args.mpq)
            if path.startswith('/__owned/file/'):
                relative = path[len('/__owned/file/'):]
                if relative not in {item['path'] for item in entries}:
                    return self.send_error(404)
                if relative in replacements:
                    return self.data(replacements[relative], 'application/json')
                file = (root / relative).resolve()
                if not file.is_relative_to(root):
                    return self.send_error(403)
                return self.local_file(file)
            if path == '/__owned/play.html':
                page = (build / 'index.html').read_text().replace('src="bootstrap.mjs"', 'src="/__owned/bootstrap.mjs"').replace('./bootstrap.mjs', '/__owned/bootstrap.mjs')
                return self.data(page.encode(), 'text/html; charset=utf-8')
            if path == '/__owned/bootstrap.mjs':
                script = (build / 'bootstrap.mjs').read_text().replace("'./", "'/")
                script = script.replace('new URL(name, import.meta.url)', 'new URL(name, location.origin + "/")')
                script = script.replace("globalThis.damnationxArgs.push('--',", "globalThis.damnationxArgs.push('--hero', 'BrowserQA', '--class', 'warrior', '--no-time-travel', '--mute');\n    globalThis.damnationxArgs.push('--',")
                script += "\ndocument.getElementById('play').click();\n"
                return self.data(script.encode(), 'text/javascript')
            return super().do_GET()

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'Open http://127.0.0.1:{args.port}/__owned/harness.html?auto=1', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()


if __name__ == '__main__':
    main()
