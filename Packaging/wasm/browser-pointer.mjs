// Browser mouse capture belongs to trusted DOM gestures, not the render loop.
export function installBrowserPointer(canvas) {
  if(globalThis.damnationxBrowserPointer)return globalThis.damnationxBrowserPointer;
  const state={desired:false,canCapture:false,toggleAllowed:false,locked:false,pending:false,error:null,deadline:0,gestureDeadline:0};
  globalThis.damnationxBrowserPointer=state;
  const now=()=>performance.now();
  const sync=()=>{state.locked=document.pointerLockElement===canvas;if(state.locked){state.pending=false;state.error=null;}};
  const fail=error=>{state.pending=false;state.gestureDeadline=0;state.error=error?.message ? String(error.message) : state.error || 'Pointer lock denied';};
  document.addEventListener('pointerlockchange',()=>{sync();state.pending=false;});
  document.addEventListener('pointerlockerror',fail);
  const request=()=>{
    sync();if(state.locked||state.pending||typeof canvas.requestPointerLock!=='function')return;
    state.pending=true;state.deadline=now()+1000;state.gestureDeadline=now()+500;state.error=null;
    try{const result=canvas.requestPointerLock();if(result?.then)result.then(()=>{sync();state.pending=false;},fail);}
    catch(error){fail(error);}
  };
  const editable=target=>!!target?.closest?.('input,textarea,select,[contenteditable="true"],[contenteditable=""]');
  canvas.addEventListener('pointerdown',event=>{if(event.isTrusted && event.button===0 && state.canCapture && !editable(event.target))request();},true);
  document.addEventListener('keydown',event=>{
    if(!event.isTrusted||event.repeat||event.ctrlKey||event.altKey||event.metaKey||editable(event.target)||!state.toggleAllowed)return;
    if((event.code==='KeyF'||event.code==='KeyT')&&!state.canCapture){
      // The native F/T action reaches the simulation on its next frame. Keep
      // this one gesture-authorized request alive until that mode arrives.
      state.gestureDeadline=now()+500;request();
    }
  },true);
  state.poll=(desired,canCapture,toggleAllowed)=>{
    sync();state.desired=desired;state.canCapture=canCapture;state.toggleAllowed=toggleAllowed;
    if(desired)state.gestureDeadline=0;
    if(!toggleAllowed)state.gestureDeadline=0;
    if(state.pending && now()>state.deadline)state.pending=false;
    if(!desired && now()>=state.gestureDeadline && document.pointerLockElement===canvas){document.exitPointerLock();state.locked=false;}
    return state.locked?1:state.pending?2:0;
  };
  return state;
}
