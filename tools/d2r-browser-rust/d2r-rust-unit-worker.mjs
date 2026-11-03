// One model per isolated worker: terminating this worker releases WASM memory.
import init,{convert_mapping_spec} from './d2r_browser_rust.js';
import {createCascBridge,ownedStoragePath} from './casc-bridge.mjs';
function rangeBlob(file) {
  if (!['127.0.0.1','localhost','[::1]'].includes(location.hostname)) throw Error('Range input is restricted to local QA');
  const url=new URL(file.url,location.href);if(url.origin!==location.origin)throw Error('Local QA origin mismatch');
  return {size:file.size,slice(start,end){end=Math.min(end,file.size);if(end<=start)return new Blob([]);const r=new XMLHttpRequest();r.open('GET',url.href,false);r.responseType='arraybuffer';r.setRequestHeader('Range',`bytes=${start}-${end-1}`);r.send();if(r.status!==206)throw Error('Range request failed: '+r.status);return new Blob([r.response]);}};
}
onmessage=async({data})=>{
  let bridge;
  try {
    const {default:create}=await import('./d2r-casc.mjs');
    const casc=await create({noInitialRun:true,locateFile:n=>new URL(n,import.meta.url).href});
    casc.FS.mkdirTree('/owned');casc.FS.mkdirTree('/scratch');
    casc.FS.mount(casc.WORKERFS,{blobs:data.files.map(f=>({name:f.path,data:f.blob||rangeBlob(f)}))},'/owned');
    bridge=createCascBridge(casc);bridge.open(ownedStoragePath(data.files.map(f=>f.path)));await init();
    const output=convert_mapping_spec(bridge,data.recipe,data.key,data.size);
    const emit=(path,bytes)=>postMessage({type:'file',path,bytes},[bytes.buffer]);
    emit(`${data.key}.glb`,output.glb);emit(`${data.key}.json`,new TextEncoder().encode(output.sidecar));
    for(const [name,bytes] of Object.entries(output.files))emit(name,bytes);
    postMessage({type:'done',sidecar:output.sidecar});
  }catch(e){postMessage({type:'error',message:e.stack||String(e)});}finally{bridge?.close();}
};
