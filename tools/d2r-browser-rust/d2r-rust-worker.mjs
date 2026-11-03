// Import owned D2R assets locally, streaming output to the browser coordinator.
import init, {recipe_json,resolve_mapping,convert_environment_category,convert_hud,convert_portal} from './d2r_browser_rust.js';
import {createCascBridge,ownedBuildName,ownedStoragePath} from './casc-bridge.mjs';
function rangeBlob(file) {
  if (!['127.0.0.1','localhost','[::1]'].includes(location.hostname)) throw Error('Range input is restricted to local QA');
  const url=new URL(file.url,location.href);if(url.origin!==location.origin)throw Error('Local QA origin mismatch');
  return {size:file.size,slice(start,end){end=Math.min(end,file.size);if(end<=start)return new Blob([]);const r=new XMLHttpRequest();r.open('GET',url.href,false);r.responseType='arraybuffer';r.setRequestHeader('Range',`bytes=${start}-${end-1}`);r.send();if(r.status!==206)throw Error('Range request failed: '+r.status);return new Blob([r.response]);}};
}
const progress=message=>postMessage({type:'progress',message});
let running=false;
onmessage=async ({data})=>{
  if(running||data.type!=='convert')return;running=true;let bridge;
  try {
    const {default:create}=await import('./d2r-casc.mjs');
    const casc=await create({noInitialRun:true,locateFile:n=>new URL(n,import.meta.url).href,printErr:progress});
    casc.FS.mkdirTree('/owned');casc.FS.mkdirTree('/scratch');
    casc.FS.mount(casc.WORKERFS,{blobs:data.files.map(f=>({name:f.path,data:f.blob||rangeBlob(f)}))},'/owned');
    const storage=ownedStoragePath(data.files.map(f=>f.path)), steam=storage!=='/owned';
    bridge=createCascBridge(casc);bridge.open(storage);await init();
    const gameBuild=ownedBuildName(casc.FS.readFile(steam?storage:'/owned/.build.info'),steam);
    const options=data.options||{}, size=Number(options.textureSize||512);
    if(![256,512,1024].includes(size))throw Error('Texture size must be 256, 512 or 1024');
    const selected=new Set(options.categories||['player','towner','monster','object','environment','ui','vfx']);
    const fetchRecipe=async name=>{const r=await fetch(new URL(name,import.meta.url));if(!r.ok)throw Error(`Missing converter recipe ${name}`);return r.text();};
    const recipe=await fetchRecipe('mapping.toml'), mapping=JSON.parse(recipe_json(recipe));
    const wanted=new Set();for(const kind of ['player','towner','monster','object','item','unique']){
      if(!selected.has(kind)&&!(kind==='unique'&&selected.has('monster'))&&!(kind==='item'&&selected.has('object')))continue;
      for(const entry of mapping[kind]||[]){if(entry.model)wanted.add(entry.model);for(const key of entry.options||[])if(key)wanted.add(key);}
    }
    if(options.onlyModels){const only=new Set(options.onlyModels);for(const key of wanted)if(!only.has(key))wanted.delete(key);}
    const sidecars={};const emit=(path,bytes)=>{if(!(bytes instanceof Uint8Array))throw Error('Invalid output bytes');postMessage({type:'file',path,bytes},[bytes.buffer]);};
    for(const key of [...wanted].sort()){
      progress(`Converting ${key}…`);
      const sidecar=await new Promise((resolve,reject)=>{
        const worker=new Worker(new URL('./d2r-rust-unit-worker.mjs',import.meta.url),{type:'module'});
        worker.onerror=e=>{worker.terminate();reject(Error(e.message||'Model worker failed'));};
        worker.onmessage=({data})=>{if(data.type==='file'){emit(data.path,data.bytes);return;}
          if(data.type==='error'){worker.terminate();reject(Error(data.message));return;}
          if(data.type==='done'){worker.terminate();resolve(data.sidecar);}};
        worker.postMessage({files:data.files,recipe,key,size});
      });
      sidecars[key]=JSON.parse(sidecar);
    }
    emit('mapping.resolved.json',new TextEncoder().encode(resolve_mapping(recipe,JSON.stringify(sidecars))));
    if(selected.has('environment')) {progress('Converting world assets…');const env=await fetchRecipe('env_mapping.toml');const manifest=convert_environment_category(bridge,env,gameBuild,size,emit);emit('env/env.resolved.json',new TextEncoder().encode(manifest));}
    if(selected.has('ui')) {progress('Converting interface…');for(const [name,bytes] of Object.entries(convert_hud(bridge,gameBuild)))emit(name,bytes);}
    if(selected.has('vfx')) {progress('Converting portal…');for(const [name,bytes] of Object.entries(convert_portal(bridge,gameBuild)))emit(name,bytes);}
    postMessage({type:'done',result:{models:wanted.size,categories:[...selected].sort(),textureSize:size}});
  } catch(e){postMessage({type:'error',message:e.stack||String(e)});} finally {bridge?.close();}
};
