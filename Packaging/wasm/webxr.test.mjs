import test from 'node:test';
import assert from 'node:assert/strict';
import {installWebXr, STATE_FLOATS} from './webxr.mjs';

test('immersive runtime stereo and Touch state drive migrated RAF, loss clears inputs, exit restores RAF', async () => {
  const native = new Map(); let next=1, xrFrame, click, ended;
  const button={style:{},addEventListener(name,fn){if(name==='click')click=fn;}};
  const canvas={style:{width:'100%',height:'100%'},width:800,height:600};
  const calls=[];
  const gl=new Proxy({}, {get(target,name){
    if(name==='getShaderParameter'||name==='getProgramParameter')return()=>true;
    if(name==='getShaderInfoLog'||name==='getProgramInfoLog')return()=>'';
    if(String(name).startsWith('create'))return()=>({});
    if(name==='makeXRCompatible')return async()=>{};
    if(name==='getUniformLocation')return()=>({});
    if(String(name).toUpperCase()===name)return 1;
    return(...args)=>calls.push([name,...args]);
  }});
  globalThis.window={devicePixelRatio:1,requestAnimationFrame(fn){const id=next++;native.set(id,fn);return id;},cancelAnimationFrame(id){native.delete(id);},dispatchEvent(){}};
  globalThis.document={querySelector:()=>canvas,body:{append(){}},createElement:name=>name==='button'?button:{getContext:()=>gl}};
  globalThis.isSecureContext=true;
  const identity={position:{x:0,y:1.7,z:0},orientation:{x:0,y:0,z:0,w:1}};
  const projection=Float32Array.from({length:16},(_,i)=>i+1);
  const session={visibilityState:'visible',inputSources:[{handedness:'left',targetRaySpace:{},gripSpace:{},gamepad:{axes:[0,0,.3,-.7],buttons:Array.from({length:6},(_,i)=>({value:i/5}))}}],
    requestReferenceSpace:async()=>({}),updateRenderState(){},requestAnimationFrame(fn){xrFrame=fn;},addEventListener(name,fn){if(name==='end')ended=fn;},async end(){ended();}};
  Object.defineProperty(globalThis,'navigator',{configurable:true,value:{xr:{isSessionSupported:async()=>true,requestSession:async()=>session}}});
  globalThis.XRWebGLLayer=class{constructor(){this.framebuffer={};}getViewport(view){return{x:view.eye==='right'?900:0,y:0,width:900,height:1000};}};
  let enabled=false;globalThis.damnationxCore={_dx_host_set_vr_enabled(value){enabled=!!value;}};
  const xr=installWebXr();await Promise.resolve();assert.equal(button.disabled,false);assert.equal(xr.state.length,STATE_FLOATS);
  let tick=0;window.requestAnimationFrame(()=>{tick++;assert.equal(xr.state[1],1);});
  assert.equal(native.size,1);await click();assert.equal(native.size,0);assert.equal(enabled,true);
  const views=['left','right'].map(eye=>({eye,transform:identity,projectionMatrix:projection}));
  xrFrame(100,{getViewerPose:()=>({transform:identity,views}),getPose:()=>({transform:identity})});
  assert.equal(tick,1);assert.equal(xr.state[4],900);assert.deepEqual([...xr.state.slice(23,39)],[...projection]);
  assert.ok(Math.abs(xr.state[80]-.3)<1e-6);assert.ok(Math.abs(xr.state[81]-.7)<1e-6);
  assert.equal(calls.filter(c=>c[0]==='drawArrays').length,0);
  canvas.width=1800;canvas.height=1000;
  xrFrame(108,{getViewerPose:()=>({transform:identity,views}),getPose:()=>({transform:identity})});
  assert.equal(calls.filter(c=>c[0]==='drawArrays').length,2);
  let suspended=0;window.requestAnimationFrame(()=>suspended++);
  xrFrame(114,{getViewerPose:()=>null});assert.equal(xr.state[1],0);assert.equal(xr.state[64],0);assert.equal(suspended,1);
  window.requestAnimationFrame(()=>{});await click();assert.equal(xr.state[0],0);assert.equal(native.size,1);assert.equal(canvas.style.width,'100%');assert.equal(button.textContent,'Enter VR');
  delete globalThis.damnationxWebXr;delete globalThis.damnationxCore;
});
