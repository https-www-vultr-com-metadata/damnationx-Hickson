// Real Chromium/WORKERFS owned-install smoke; assets are served over loopback
// using byte ranges and never leave the machine. This tests one model only.
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const [stageArg, cascArg, gameArg, chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', mode = 'primitives'] = process.argv.slice(2);
if (!stageArg || !cascArg || !gameArg) throw Error('usage: node owned-browser-smoke.mjs WASM_STAGE CASC_BUILD OWNED_GAME_DIR [CHROME]');
const stage = path.resolve(stageArg), casc = path.resolve(cascArg), game = path.resolve(gameArg);
const here = path.dirname(fileURLToPath(import.meta.url));
const assetFiles = [];
function walk(dir, prefix) {
  for (const entry of fs.readdirSync(dir, {withFileTypes:true})) {
    const full = path.join(dir,entry.name), relative = prefix + entry.name;
    if (entry.isDirectory()) walk(full, relative + '/');
    else if (entry.isFile()) assetFiles.push({path: relative, size: fs.statSync(full).size, full});
  }
}
assetFiles.push({path:'.build.info', size:fs.statSync(path.join(game,'.build.info')).size, full:path.join(game,'.build.info')});
walk(path.join(game,'Data'), 'Data/');
const descriptors = assetFiles.map(({path,size}, i) => ({path,size,url:'/data/'+i}));
const worker = `
import create from './d2r-casc.mjs';
import init, {recipe_json, convert_model, convert_environment, convert_environment_material, convert_hud, convert_portal} from './d2r_browser_rust.js';
import {createCascBridge} from './casc-bridge.mjs';
function blob(file) {return {size:file.size,slice(start,end) {end=Math.min(end,file.size);if(end<=start)return new Blob([]);const r=new XMLHttpRequest();r.open('GET',file.url,false);r.responseType='arraybuffer';r.setRequestHeader('Range', 'bytes='+start+'-'+(end-1));r.send();if(r.status!==206)throw Error('range failed '+r.status);return new Blob([r.response]);}};}
onmessage=async({data})=>{try {
  const casc=await create({noInitialRun:true,locateFile:n=>new URL(n,import.meta.url).href});
  casc.FS.mkdirTree('/owned');casc.FS.mkdirTree('/scratch');
  casc.FS.mount(casc.WORKERFS,{blobs:data.map(f=>({name:f.path,data:blob(f)}))},'/owned');
  const bridge=createCascBridge(casc);bridge.open();
  try {await init();const model=convert_model(bridge,'data/hd/character/enemy/zombie1.json','{}',false);
    try {const glb=model.glb,meta=JSON.parse(model.metadata);if(new TextDecoder().decode(glb.slice(0,4))!=='glTF'||!meta.meshes||!meta.bones)throw Error('invalid model');
      const fountain=convert_environment(bridge,JSON.stringify({parts:[{model:'data/hd/env/model/act1/tristram/act1_tristram_props/stone_fountain01.model'},{model:'data/hd/env/model/act1/tristram/act1_tristram_props/fountain01.model'}]}),0);
      try {const env=fountain.glb,info=JSON.parse(fountain.metadata);if(new TextDecoder().decode(env.slice(0,4))!=='glTF'||!info.triangles)throw Error('invalid environment');
        const material=convert_environment_material(bridge,JSON.stringify({biome:'act1_tristram',layer:'tristram_grass01'}),256);const materialInfo=JSON.parse(material.manifest);if(!materialInfo.albedo || !material.files[materialInfo.albedo])throw Error('missing terrain albedo');
        const recipes=JSON.parse(recipe_json(await (await fetch('/env_mapping.toml')).text()));const cottage=convert_environment(bridge,JSON.stringify(recipes.models.cottage1),0);
        let cottageInfo;try {const bytes=cottage.glb,view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);const json=JSON.parse(new TextDecoder().decode(bytes.slice(20,20+view.getUint32(12,true))));
          const roof=json.materials.find(x=>x.name==='act1_outdoors_cottages_roof_01_d');if(!roof || roof.alphaMode!=='MASK' || !roof.doubleSided || roof.pbrMetallicRoughness?.metallicFactor!==1)throw Error('cottage roof material is wrong');
          cottageInfo={bytes:bytes.length,...JSON.parse(cottage.metadata)};
        }finally{cottage.free();}
        const hud=convert_hud(bridge,'browser-smoke');if(!hud['ui/ui.resolved.json'])throw Error('missing HUD manifest');
        const portal=convert_portal(bridge,'browser-smoke');if(!portal['vfx/town_portal/portal.resolved.json'])throw Error('missing portal manifest');
        postMessage({ok:true,model:{bytes:glb.length,...meta},environment:{bytes:env.length,...info},cottage:cottageInfo,terrainFiles:Object.keys(material.files).length,hudFiles:Object.keys(hud).length,portalFiles:Object.keys(portal).length});
      }finally{fountain.free();}
    }finally{model.free();}
  } finally {bridge.close();}
} catch(e){postMessage({ok:false,error:e.stack||String(e)});}};`;
const atomicPage = `<!doctype html><meta charset="utf-8"><script type="module">
import {browserAssets} from '/local-assets.mjs';import {convertD2rInstall} from '/d2r-converter.mjs';
function finish(data){fetch('/result',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});}
try {
 const prior=new File([JSON.stringify({entries:[]})],'mapping.resolved.json');Object.defineProperty(prior,'webkitRelativePath',{value:'glb/mapping.resolved.json'});
 await browserAssets.importFolder([prior],'d2r');const before=browserAssets.summary();
 let instance;globalThis.Worker=class {constructor(){instance=this;}postMessage(){}terminate(){this.stopped=true;} send(data){this.onmessage({data});}};
 function input(path){const f=new File(['x'],path.split('/').at(-1));Object.defineProperty(f,'webkitRelativePath',{value:'game/'+path});return f;}
 const controller=new AbortController();const pending=convertD2rInstall([input('.build.info'),input('Data/data/x.idx')],{signal:controller.signal});
 instance.send({type:'file',path:'mapping.resolved.json',bytes:new TextEncoder().encode('{"entries":[]}')});controller.abort();
 let cancelled=false;try{await pending;}catch(e){cancelled=/cancelled/i.test(String(e));}
 const after=browserAssets.summary();await browserAssets.restore();const restored=browserAssets.summary();
 finish({ok:cancelled&&instance.stopped&&before.files===after.files&&before.bytes===after.bytes&&restored.files===before.files,before,after,restored});
} catch(e){finish({ok:false,error:e.stack||String(e)});}</script>`;
const page = mode==='atomic' ? atomicPage : mode.startsWith('worker') ? `<!doctype html><meta charset="utf-8"><script type="module">
import {browserAssets} from '/local-assets.mjs';
const worker=new Worker('/d2r-rust-worker.mjs',{type:'module'});const files=new Map();
function finish(result){fetch('/result',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(result)});}
worker.onmessage=async({data})=>{if(data.type==='progress'){fetch('/progress',{method:'POST',body:data.message});return;}if(data.type==='file'){const f=new File([data.bytes],data.path.split('/').at(-1));Object.defineProperty(f,'webkitRelativePath',{value:'glb/'+data.path});files.set(data.path,f);return;}if(data.type==='error'){finish({ok:false,error:data.message,files:files.size});return;}if(data.type==='done'){const required=${JSON.stringify(mode==='worker-env'?['env/env.resolved.json']:mode==='worker-full'?['zombie.glb','mapping.resolved.json','env/env.resolved.json','ui/ui.resolved.json','vfx/town_portal/portal.resolved.json']:['zombie.glb','mapping.resolved.json','ui/ui.resolved.json','vfx/town_portal/portal.resolved.json'])};const missing=required.filter(x=>!files.has(x));if(missing.length){finish({ok:false,missing,files:files.size});return;}try{const summary=await browserAssets.importFolder([...files.values()],'d2r');finish({ok:true,files:files.size,summary,result:data.result});}catch(e){finish({ok:false,error:e.stack||String(e),files:files.size});}}};
worker.onerror=e=>finish({ok:false,error:e.message});worker.postMessage({type:'convert',files:${JSON.stringify(descriptors)},options:${JSON.stringify(mode==='worker-env'?{categories:['environment'],textureSize:256}:mode==='worker-full'?{textureSize:256}:{categories:['monster','ui','vfx'],onlyModels:['zombie'],textureSize:256})}});</script>` : `<!doctype html><meta charset="utf-8"><script>const worker=new Worker('/worker.mjs',{type:'module'});worker.onmessage=e=>fetch('/result',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(e.data)});worker.onerror=e=>fetch('/result',{method:'POST',body:JSON.stringify({ok:false,error:e.message})});worker.postMessage(${JSON.stringify(descriptors)});</script>`;
const staticFiles = new Map([
  ['/worker.mjs', {bytes:Buffer.from(worker), type:'text/javascript'}],
  ['/casc-bridge.mjs', {file:path.join(here,'casc-bridge.mjs'), type:'text/javascript'}],
  ['/local-assets.mjs', {file:path.join(here,'../../Packaging/wasm/local-assets.mjs'), type:'text/javascript'}],
  ['/d2r-converter.mjs', {file:path.join(here,'../../Packaging/wasm/d2r-converter.mjs'), type:'text/javascript'}],
  ...['d2r-casc.mjs','d2r-casc.wasm'].map(name=>['/'+name,{file:path.join(casc,name),type:name.endsWith('.wasm')?'application/wasm':'text/javascript'}]),
  ...['d2r_browser_rust.js','d2r_browser_rust_bg.wasm'].map(name=>['/'+name,{file:path.join(stage,name),type:name.endsWith('.wasm')?'application/wasm':'text/javascript'}]),
  ...['d2r-rust-worker.mjs','d2r-rust-unit-worker.mjs','mapping.toml','env_mapping.toml'].map(name=>['/'+name,{file:path.join(stage,name),type:name.endsWith('.mjs')?'text/javascript':'text/plain'}]),
]);
let finish;
const result = new Promise(resolve => finish = resolve);
const server = http.createServer((req,res)=>{
  const url = new URL(req.url,'http://localhost');
  if(url.pathname==='/'){res.writeHead(200,{'Content-Type':'text/html'});return res.end(page);}
  if(url.pathname==='/result' && req.method==='POST'){let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{res.end('ok');finish(JSON.parse(body));});return;}
  if(url.pathname==='/progress' && req.method==='POST'){let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{console.log(body);res.end('ok');});return;}
  const match = /^\/data\/(\d+)$/.exec(url.pathname);
  if(match){const file=assetFiles[Number(match[1])];const range=/^bytes=(\d+)-(\d+)$/.exec(req.headers.range||'');if(!file||!range){res.writeHead(400);return res.end();}const start=Number(range[1]),end=Math.min(Number(range[2]),file.size-1);if(start>end||start>=file.size){res.writeHead(416);return res.end();}res.writeHead(206,{'Content-Length':end-start+1,'Content-Range':`bytes ${start}-${end}/${file.size}`,'Accept-Ranges':'bytes'});return fs.createReadStream(file.full,{start,end}).pipe(res);}
  const item=staticFiles.get(url.pathname);if(!item){res.writeHead(404);return res.end();}res.writeHead(200,{'Content-Type':item.type});if(item.bytes)return res.end(item.bytes);fs.createReadStream(item.file).pipe(res);
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const profile=fs.mkdtempSync('/tmp/d2r-browser-chrome-');
const child=spawn(chrome,['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+profile,`http://127.0.0.1:${server.address().port}/`],{stdio:'ignore'});
try {
  const verdict=await Promise.race([result,new Promise((_,reject)=>{const timer=setTimeout(()=>reject(Error('browser smoke timed out')),1200000);timer.unref();})]);
  console.log(JSON.stringify(verdict));if(!verdict.ok)process.exitCode=1;
} finally {child.kill();server.close();fs.rmSync(profile,{recursive:true,force:true,maxRetries:10,retryDelay:100});}
