import {browserAssets} from './local-assets.mjs';
export function convertD2rInstall(fileList, options={}, progress=()=>{}) {
  if(options.signal?.aborted)return Promise.reject(new Error('D2R conversion cancelled'));
  const files=[];
  for(const file of fileList){
    const path=file.webkitRelativePath?file.webkitRelativePath.split('/').slice(1).join('/'):file.name;
    if(path.split('/').some(part=>part==='..'||part==='.'))return Promise.reject(new Error('Invalid install file path'));
    if(path==='.build.info'||/^Data\//i.test(path))files.push({path,blob:file});
  }
  // Battle.net: .build.info and Data/data indexes. Steam (no .build.info): Data/.build.config and its .data archives.
  const battleNet=files.some(file=>file.path==='.build.info')&&files.some(file=>/^Data\/data\/.*\.idx$/i.test(file.path));
  const steam=!files.some(file=>file.path==='.build.info')&&files.some(file=>/^Data\/\.build\.config$/i.test(file.path))&&files.some(file=>/^Data\/[^/]+\.data$/i.test(file.path));
  if(!battleNet&&!steam)return Promise.reject(new Error('Choose the D2R install folder: a Battle.net install contains .build.info and Data/data, a Steam install contains Data/.build.config.'));
  return new Promise((resolve,reject)=>{
    const worker=new Worker(new URL('./d2r-converter-worker.mjs',import.meta.url),{type:'module'});
    const outputs=new Map();
    let committing=false;
    const stop=error=>{worker.terminate();reject(error);};
    worker.onerror=event=>stop(new Error(event.message||'The D2R conversion worker stopped'));
    worker.onmessage=async ({data})=>{
      if(data.type==='progress'){progress(data.message);return;}
      if(data.type==='error'){stop(new Error(data.message));return;}
      if(data.type==='file'){
        const file=new File([data.bytes],data.path.split('/').at(-1));
        Object.defineProperty(file,'webkitRelativePath',{value:'glb/'+data.path});outputs.set(data.path,file);return;
      }
      if(data.type==='done'){
        committing=true;
        worker.terminate();
        try{
          // A conversion commits as one validated replacement, preserving a previous import on failure.
          const summary=await browserAssets.importFolder([...outputs.values()],'d2r',progress,options.signal);
          resolve({summary,...data.result});
        }catch(error){reject(error);}
      }
    };
    const {signal, ...workerOptions} = options;
    worker.postMessage({type:'convert',files,options:workerOptions});
    if(options.signal){
      options.signal.addEventListener('abort',()=>{if(!committing)stop(new Error('D2R conversion cancelled'));},{once:true});
    }
  });
}

export async function filesFromD2rDirectory(handle) {
  const files=[];
  async function walk(directory, prefix) {
    for await(const child of directory.values()) {
      const relative=prefix+child.name;
      if(child.kind==='directory') {
        if(relative.toLowerCase()==='data'||relative.toLowerCase().startsWith('data/'))await walk(child,relative+'/');
      } else if(relative==='.build.info'||relative.toLowerCase().startsWith('data/')) {
        const file=await child.getFile();
        Object.defineProperty(file,'webkitRelativePath',{value:handle.name+'/'+relative});files.push(file);
      }
    }
  }
  await walk(handle,'');
  return files;
}
