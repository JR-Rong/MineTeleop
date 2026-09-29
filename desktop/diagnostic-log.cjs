'use strict';
const fs=require('node:fs/promises');
// The Electron process drains native pipes immediately; serialized asynchronous
// writes must not hold up the native control loop or grow without a bound.
function createDiagnosticLog(file,{maxBytes=8*1024*1024,files=3,maxQueuedBytes=1024*1024}={}){
  let pending=Promise.resolve(),queued=0,dropped=0;
  function write(event,details={}){
    const line=JSON.stringify({event,logged_at_utc_ms:Date.now(),...details})+'\n';
    const bytes=Buffer.byteLength(line);
    if(queued+bytes>maxQueuedBytes){dropped++;return;}
    queued+=bytes;
    pending=pending.then(async()=>{
      const stat=await fs.stat(file).catch(error=>{if(error.code==='ENOENT')return {size:0};throw error;});
      if(stat.size+bytes>maxBytes){
        for(let i=files-1;i>=1;i--){
          const source=i===1?file:`${file}.${i-1}`,target=`${file}.${i}`;
          await fs.rm(target,{force:true});
          await fs.rename(source,target).catch(error=>{if(error.code!=='ENOENT')throw error;});
        }
      }
      if(dropped){await fs.appendFile(file,JSON.stringify({event:'desktop_log_dropped',logged_at_utc_ms:Date.now(),count:dropped})+'\n');dropped=0;}
      await fs.appendFile(file,line);
    }).catch(()=>{dropped++;}).finally(()=>{queued-=bytes;});
  }
  return {write,flush:()=>pending};
}
module.exports={createDiagnosticLog};
