'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs/promises');
const os=require('node:os');
const path=require('node:path');
const {createDiagnosticLog}=require('../diagnostic-log.cjs');
test('native diagnostic journal rotates complete records and reports queue loss',async t=>{
  const directory=await fs.mkdtemp(path.join(os.tmpdir(),'mine-diagnostic-log-'));
  t.after(()=>fs.rm(directory,{recursive:true,force:true}));
  const file=path.join(directory,'desktop-events.jsonl');
  const journal=createDiagnosticLog(file,{maxBytes:400,files:3,maxQueuedBytes:500});
  for(let i=0;i<8;i++){journal.write('native_stdout',{message:`line-${i}`});await journal.flush();}
  const names=await fs.readdir(directory);assert.ok(names.length>1);
  for(const name of names){
    for(const line of (await fs.readFile(path.join(directory,name),'utf8')).trim().split('\n')){
      const record=JSON.parse(line);assert.equal(typeof record.logged_at_utc_ms,'number');
    }
  }
  journal.write('too_large',{message:'x'.repeat(1000)});journal.write('after_drop');await journal.flush();
  const current=await fs.readFile(file,'utf8');assert.match(current,/desktop_log_dropped/);assert.match(current,/after_drop/);
});
