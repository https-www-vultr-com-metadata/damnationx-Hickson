// WebGPU canvas -> WebGL XRWebGLLayer. Frame N presents frame N-1; the
// runtime alone supplies stereo poses/projections. No synthetic eye offsets.
export const STATE_FLOATS = 160;
export function installWebXr({canvas = document.querySelector('#canvas'), renderScale = .8} = {}) {
  if (globalThis.damnationxWebXr) return globalThis.damnationxWebXr;
  if (!canvas) throw new Error('WebXR requires the gameplay canvas');
  const state = new Float32Array(STATE_FLOATS);
  const xr = {state, session: null, error: null, renderScale, canvas};
  globalThis.damnationxWebXr = xr;
  const nativeRaf = window.requestAnimationFrame.bind(window);
  const nativeCancel = window.cancelAnimationFrame.bind(window);
  const queued = new Map(), nativePending = new Map(); let nextId = -1;
  window.requestAnimationFrame = callback => {
    const id = nextId--;
    if (xr.session) queued.set(id, callback);
    else nativePending.set(id, {callback, handle: nativeRaf(time => { nativePending.delete(id); callback(time); })});
    return id;
  };
  window.cancelAnimationFrame = id => {
    queued.delete(id); const pending=nativePending.get(id);
    if(pending){nativeCancel(pending.handle);nativePending.delete(id);}
  };
  const button = document.createElement('button');
  button.id = 'damnationx-enter-vr'; button.textContent = 'Checking VR…'; button.disabled = true;
  button.style.cssText = 'position:fixed;right:16px;bottom:16px;z-index:10000;padding:12px 20px';
  document.body.append(button);
  const pose = (offset, transform) => {
    const p = transform.position, q = transform.orientation;
    state.set([p.x,p.y,p.z,q.x,q.y,q.z,q.w], offset);
  };
  function cleanup(session) {
    if (xr.session !== session) return;
    xr.session = null; state.fill(0); button.textContent = 'Enter VR'; button.disabled = false;
    canvas.style.width = xr.oldStyle.width; canvas.style.height = xr.oldStyle.height;
    // Winit sees the original CSS size again and recreates its surface.
    window.dispatchEvent(new Event('resize'));
    const callbacks = [...queued.entries()]; queued.clear();
    for (const [id,callback] of callbacks) nativePending.set(id,{callback,handle:nativeRaf(time=>{nativePending.delete(id);callback(time);})});
    if (xr.gl) {
      xr.gl.deleteTexture(xr.texture); xr.gl.deleteBuffer(xr.buffer); xr.gl.deleteProgram(xr.program);
    }
    xr.gl = null;
  }
  async function start() {
    if (xr.session) { await xr.session.end(); return; }
    button.disabled = true;
    let session;
    try {
      session = await navigator.xr.requestSession('immersive-vr', {optionalFeatures:['local-floor']});
      // Enter VR is the browser equivalent of native --vr: enable the saved
      // option only after the user actually obtains an immersive session.
      globalThis.damnationxCore?._dx_host_set_vr_enabled?.(1);
      xr.oldStyle = {width: canvas.style.width, height: canvas.style.height};
      xr.session = session;
      for(const [id,pending] of nativePending){nativeCancel(pending.handle);queued.set(id,pending.callback);}
      nativePending.clear();
      session.addEventListener('end', () => cleanup(session), {once:true});
      const gl = document.createElement('canvas').getContext('webgl2', {xrCompatible:true, alpha:false, antialias:false});
      if (!gl) throw new Error('WebGL2 is required for the WebXR presentation bridge');
      xr.gl = gl; await gl.makeXRCompatible();
      const layer = new XRWebGLLayer(session, gl, {antialias:false, depth:false, framebufferScaleFactor: xr.renderScale});
      session.updateRenderState({baseLayer:layer, depthNear:.05, depthFar:500});
      let reference, noFloor = 0;
      try { reference = await session.requestReferenceSpace('local-floor'); }
      catch { reference = await session.requestReferenceSpace('local'); noFloor = 1; }
      const shader = (type, source) => {
        const s = gl.createShader(type); gl.shaderSource(s,source); gl.compileShader(s);
        if (!gl.getShaderParameter(s,gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
      };
      const vs = shader(gl.VERTEX_SHADER, `#version 300 es\nin vec2 p;out vec2 uv;uniform vec4 rect;void main(){uv=vec2(rect.x+(p.x*.5+.5)*rect.z,1.-(p.y*.5+.5));gl_Position=vec4(p,0,1);}`);
      const fs = shader(gl.FRAGMENT_SHADER, `#version 300 es\nprecision mediump float;in vec2 uv;out vec4 color;uniform sampler2D image;void main(){color=vec4(texture(image,uv).rgb,1);}`);
      const program = xr.program = gl.createProgram(); gl.attachShader(program,vs); gl.attachShader(program,fs);
      gl.bindAttribLocation(program,0,'p'); gl.linkProgram(program); gl.deleteShader(vs); gl.deleteShader(fs);
      if (!gl.getProgramParameter(program,gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
      const buffer = xr.buffer = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER,buffer);
      gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,3,-1,-1,3]),gl.STATIC_DRAW);
      const texture = xr.texture = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D,texture);
      for (const [p,v] of [[gl.TEXTURE_MIN_FILTER,gl.LINEAR],[gl.TEXTURE_MAG_FILTER,gl.LINEAR],[gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE],[gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE]]) gl.texParameteri(gl.TEXTURE_2D,p,v);
      let last = 0, stereoReady = false;
      const frame = (time, frame) => {
        if (xr.session !== session) return;
        session.requestAnimationFrame(frameLoop);
        state.fill(0); state[0]=1; state[2]=last ? Math.min(.1,(time-last)/1000) : 1/72; last=time; state[3]=noFloor;
        const viewer = frame.getViewerPose(reference);
        const visible = viewer && session.visibilityState === 'visible' && viewer.views.length === 2;
        if (visible) {
          state[1]=1; pose(8,viewer.transform);
          // Equal source viewports; destination viewports remain runtime-owned.
          const width = Math.max(...viewer.views.map(v=>layer.getViewport(v).width));
          const height = Math.max(...viewer.views.map(v=>layer.getViewport(v).height));
          state[4]=width; state[5]=height;
          const ratio = window.devicePixelRatio || 1;
          if (canvas.width !== width*2 || canvas.height !== height) {
            canvas.style.width=`${width*2/ratio}px`; canvas.style.height=`${height/ratio}px`;
            window.dispatchEvent(new Event('resize'));
          }
          for (const view of viewer.views) {
            const eye=view.eye==='right'?1:0, base=16+eye*24;
            pose(base,view.transform); state.set(view.projectionMatrix,base+7);
          }
          for (const source of session.inputSources) {
            const hand=source.handedness==='left'?0:source.handedness==='right'?1:-1;
            if (hand<0) continue;
            const base=64+hand*40, aim=frame.getPose(source.targetRaySpace,reference);
            const grip=source.gripSpace && frame.getPose(source.gripSpace,reference);
            if (aim) {state[base]=1;pose(base+1,aim.transform);}
            if (grip) {state[base+8]=1;pose(base+9,grip.transform);}
            const pad=source.gamepad;
            if (pad) {
              state[base+16]=pad.axes[2]||0;state[base+17]=-(pad.axes[3]||0);
              for(let i=0;i<6;i++) state[base+18+i]=pad.buttons[i]?.value||0;
            }
          }
          gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
          gl.useProgram(program);gl.bindBuffer(gl.ARRAY_BUFFER,buffer);gl.enableVertexAttribArray(0);gl.vertexAttribPointer(0,2,gl.FLOAT,false,0,0);
          gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,texture);
          if(stereoReady && canvas.width===width*2 && canvas.height===height){
          gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,canvas);
          for(const view of viewer.views){const v=layer.getViewport(view);gl.viewport(v.x,v.y,v.width,v.height);gl.uniform4f(gl.getUniformLocation(program,'rect'),view.eye==='right'?.5:0,0,.5,1);gl.drawArrays(gl.TRIANGLES,0,3);}
          }
        }
        stereoReady = !!visible;
        // State is available before Bevy reads it. The canvas was copied first.
        const callbacks=[...queued.values()];queued.clear();for(const callback of callbacks)callback(time);
      };
      const frameLoop = (time, value) => {
        try { frame(time,value); }
        catch(error) { xr.error=String(error); console.error('WebXR presentation failed',error); session.end(); }
      };
      button.textContent='Exit VR';button.disabled=false;session.requestAnimationFrame(frameLoop);
    } catch(error) {
      xr.error=String(error);console.error('WebXR entry failed',error);
      if(session){await session.end().catch(()=>{});cleanup(session);}
      button.textContent='Enter VR';button.disabled=false;
    }
  }
  button.addEventListener('click', start);
  if (navigator.xr && isSecureContext) navigator.xr.isSessionSupported('immersive-vr').then(supported=>{
    button.textContent=supported?'Enter VR':'VR unavailable';button.disabled=!supported;
  }).catch(error=>{xr.error=String(error);button.textContent='VR unavailable';});
  else button.textContent='VR unavailable';
  return xr;
}
