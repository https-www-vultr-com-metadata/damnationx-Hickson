// Exercise the actual browser importer coordinator with a fake worker/store.
import assert from 'node:assert/strict';
import { browserAssets } from '../../Packaging/wasm/local-assets.mjs';
import { convertD2rInstall } from '../../Packaging/wasm/d2r-converter.mjs';
let current;
class FakeWorker {
  constructor() { current=this; this.terminated=false; }
  postMessage(data) { this.request=data; }
  terminate() { this.terminated=true; }
  send(data) { this.onmessage({data}); }
}
globalThis.Worker=FakeWorker;
let commits=0;
browserAssets.importFolder=async files=>{commits++;return {files:files.length};};
function file(name,path){const f=new File([new Uint8Array([1])],name);Object.defineProperty(f,'webkitRelativePath',{value:'D2R/'+path});return f;}
const input=[file('.build.info','.build.info'),file('000.idx','Data/data/000.idx')];
const progress=()=>{};
let task=convertD2rInstall(input,{},progress);let worker=current;
worker.send({type:'file',path:'partial.glb',bytes:new Uint8Array([1])});
worker.send({type:'error',message:'bad model'});
await assert.rejects(task,/bad model/);assert.equal(commits,0);assert.equal(worker.terminated,true);
const controller=new AbortController();task=convertD2rInstall(input,{signal:controller.signal},progress);worker=current;
worker.send({type:'file',path:'partial.glb',bytes:new Uint8Array([1])});controller.abort();
await assert.rejects(task,/cancelled/);assert.equal(commits,0);assert.equal(worker.terminated,true);
task=convertD2rInstall(input,{},progress);worker=current;
worker.send({type:'file',path:'mapping.resolved.json',bytes:new TextEncoder().encode('{}')});
worker.send({type:'done',result:{models:1}});
const result=await task;assert.equal(result.summary.files,1);assert.equal(result.models,1);assert.equal(commits,1);assert.equal(worker.terminated,true);
console.log('Browser importer cancellation and commit-gating tests passed');
