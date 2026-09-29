'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {once}=require('node:events');
const os=require('node:os');
const path=require('node:path');
const fs=require('node:fs');
const {spawn}=require('node:child_process');
const {NativeController}=require('../native-controller.cjs');
const root=process.env.MINE_TELEOP_TEST_NATIVE_ROOT;
const signalingBinary=process.env.MINE_TELEOP_TEST_SIGNALING_BINARY;
for(const mode of ['shutdown','owner-eof',...(process.platform==='win32'?[]:['sigterm'])]){
  test(`real bundled service releases its port on ${mode}`,{skip:!root,timeout:15000},async t=>{
    const controller=new NativeController({root,logPath:path.join(os.tmpdir(),'mine-desktop-lifecycle-test.jsonl')});
    t.after(()=>controller.stop());
    const url=await controller.start();
    assert.equal((await (await fetch(url+'health')).json()).status,'ok');
    assert.equal((await (await fetch(url+'api/status')).json()).authenticated,false);
    const exit=once(controller,'exit');
    if(mode==='shutdown')assert.equal((await controller.stop()).forced,false);
    else if(mode==='owner-eof')controller.child.stdin.end();
    else controller.child.kill('SIGTERM');
    const [result]=await exit;
    assert.equal(result.code,0);
    await assert.rejects(fetch(url+'health'));
  });
}

// Use both real binaries: a mocked child cannot expose HTTP stop joining a
// connect handler which is still waiting for mobile approval.
for(const mode of ['shutdown','owner-eof',...(process.platform==='win32'?[]:['sigterm'])]){
  test(`real bundled service cancels pending approval on ${mode}`,{skip:!root||!signalingBinary,timeout:20000},async t=>{
    const fixture=fs.mkdtempSync(path.join(os.tmpdir(),'mine-desktop-approval-exit-'));
    let controller,cloud;
    t.after(async()=>{
      if(controller)await controller.stop();
      if(cloud?.pid&&cloud.exitCode===null&&cloud.signalCode===null){
        const exited=once(cloud,'exit');cloud.kill();await exited;
      }
      fs.rmSync(fixture,{recursive:true,force:true});
    });
    fs.mkdirSync(path.join(fixture,'secrets'));
    fs.writeFileSync(path.join(fixture,'secrets/app-token'),'fixture-approver');
    fs.writeFileSync(path.join(fixture,'secret'),'fixture-secret');
    const identity=path.join(fixture,'identity.yaml');
    fs.writeFileSync(identity,'auth:\n  mobile_approval_timeout_ms: 120000\n  drivers:\n    - id: fixture-driver\n      password_file: secret\n      vehicles: [fixture-vehicle]\n  vehicles:\n    - id: fixture-vehicle\n      device_token_file: secret\n      mobile_approval_required: true\n');
    cloud=spawn(signalingBinary,['--config',identity,'--host','127.0.0.1','--port','0'],{stdio:['ignore','pipe','pipe']});
    const startup=await new Promise((resolve,reject)=>{
      let buffer='',errors='';
      const timer=setTimeout(()=>reject(Error(`signaling startup timeout: ${errors}`)),5000);
      cloud.once('error',error=>{clearTimeout(timer);reject(error);});
      cloud.once('exit',code=>{clearTimeout(timer);reject(Error(`signaling exited (${code}): ${errors}`));});
      cloud.stderr.on('data',chunk=>{errors=(errors+chunk).slice(-4096);});
      cloud.stdout.on('data',chunk=>{
        buffer+=chunk;let newline;
        while((newline=buffer.indexOf('\n'))!==-1){
          const line=buffer.slice(0,newline);buffer=buffer.slice(newline+1);
          let value;try{value=JSON.parse(line);}catch{continue;}
          if(value.event==='signaling_server_started'){clearTimeout(timer);resolve(value);}
        }
      });
    });
    const cloudUrl=`http://127.0.0.1:${startup.port}`;
    const request=async(url,body,token)=>{
      const response=await fetch(url,{method:body===undefined?'GET':'POST',
        headers:{'Content-Type':'application/json',...(token?{'X-Mine-Teleop-Approver-Token':token}:{})},
        body:body===undefined?undefined:JSON.stringify(body)});
      return {status:response.status,body:await response.json()};
    };
    assert.equal((await request(cloudUrl+'/vehicles/online',{vehicle_id:'fixture-vehicle',device_token:'fixture-secret',connection_id:'fixture'})).status,200);
    const approver=await request(cloudUrl+'/mobile/api/login',{password:'fixture-approver'});
    assert.equal(approver.status,200);
    const nativeRoot=path.join(fixture,'controller');
    fs.mkdirSync(path.join(nativeRoot,'config'),{recursive:true});
    for(const directory of ['bin','lib','certs','assets']){
      const source=path.join(path.resolve(root),directory);
      if(fs.existsSync(source))fs.symlinkSync(source,path.join(nativeRoot,directory),'junction');
    }
    fs.writeFileSync(path.join(nativeRoot,'config/driver-console.yaml'),`cloud:\n  signaling_url: ws://127.0.0.1:${startup.port}/signaling\n`);
    controller=new NativeController({root:nativeRoot,logPath:path.join(fixture,'browser.jsonl')});
    const local=await controller.start();
    assert.equal((await request(local+'api/login',{driver_id:'fixture-driver',password:'fixture-secret'})).status,200);
    const connecting=request(local+'api/connect',{vehicle_id:'fixture-vehicle'}).catch(error=>({error:error.message}));
    let pending;
    for(let i=0;i<150;i++){
      pending=(await request(local+'api/status')).body.pending_mobile_approval;
      if(pending?.request_id)break;
      await new Promise(resolve=>setTimeout(resolve,20));
    }
    assert.ok(pending?.request_id,'connect never entered approval wait');
    const exit=once(controller,'exit');
    const started=performance.now();
    const deadline=setTimeout(()=>controller.child.kill('SIGKILL'),5000);
    let stopped;
    try{
      if(mode==='shutdown')stopped=await controller.stop();
      else if(mode==='owner-eof')controller.child.stdin.end();
      else controller.child.kill('SIGTERM');
      const [result]=await exit;
      assert.equal(result.code,0,'pending approval prevented graceful exit');
    }finally{clearTimeout(deadline);}
    if(stopped)assert.equal(stopped.forced,false);
    const elapsed=Math.round(performance.now()-started);
    assert.ok(elapsed<5000,`shutdown took ${elapsed} ms`);
    await connecting;
    await assert.rejects(fetch(local+'health'));
    const inbox=await request(cloudUrl+'/mobile/api/requests',undefined,approver.body.token);
    assert.equal(inbox.body.requests.find(r=>r.request_id===pending.request_id)?.state,'cancelled');
    assert.equal((await request(cloudUrl+`/mobile/api/requests/${pending.request_id}/decision`,{decision:'approve'},approver.body.token)).status,409);
    assert.equal((await request(cloudUrl+'/health')).body.active_sessions,0);
    t.diagnostic(`graceful exit in ${elapsed} ms; approval cancelled; late approval rejected`);
  });
}
