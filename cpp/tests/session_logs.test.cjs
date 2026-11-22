'use strict';
const assert = require('assert').strict;
const fs = require('fs'),path = require('path'),vm = require('vm');
const {exportStatusMessage} = require('../web/session_logs.js');
let failed = 0,passed=0;const pending=[],unfinished=new Set();
function test(name, body) {
  const pass=()=>{unfinished.delete(name);passed++;console.log('[PASS] '+name)},fail=error=>{unfinished.delete(name);failed++;console.error('[FAIL] '+name,error)};
  try { const value=body();if(value&&typeof value.then==='function'){unfinished.add(name);pending.push(Promise.resolve(value).then(pass,fail))}else pass() }
  catch (error) { fail(error); }
}

test('shows whether the vehicle claimed a task and made upload progress', () => {
  assert.match(exportStatusMessage({state: 'collecting'}), /等待车端/);
  assert.match(exportStatusMessage({state: 'collecting', vehicle_progress: {claimed: true, bytes_received: 2097152}}), /2\.0 MB/);
});
test('an empty vehicle export is explicitly incomplete and gives the failure stage', () => {
  for (const [reason, text] of [['vehicle_worker_unresponsive', '未响应'], ['vehicle_upload_failed', '上传失败'], ['vehicle_upload_timeout', '超时']]) {
    const message = exportStatusMessage({state: 'ready', complete: false, manifest: {vehicle: {status: 'partial', reason, files: []}}});
    assert.match(message, /仅收集到部分日志/);
    assert.ok(message.includes(text));
  }
});
test('reports missing required files and treats optional logs separately', () => {
  const message = exportStatusMessage({state: 'ready', complete: false, manifest: {vehicle: {status: 'partial', files: [{name: 'runtime.log', status: 'partial'}]}}});
  assert.match(message, /runtime\.log/);
  assert.match(exportStatusMessage({state: 'ready', complete: true}), /收集完成/);
});
function dialogFixture(exported) {
  const calls=[],downloads=[],timers=new Map();let timerId=0;
  class Element {
    constructor(tag){this.tag=tag;this.children=[];this.value='';this.listeners=new Map();this.hidden=false;this.disabled=false;this.open=false}
    appendChild(child){this.children.push(child);if(this.tag==='select'&&!this.value)this.value=child.value}
    replaceChildren(){this.children=[];this.value=''}
    addEventListener(type,callback){this.listeners.set(type,callback)}
    showModal(){this.open=true}
    close(){this.open=false;const callback=this.listeners.get('close');if(callback)callback()}
    click(){downloads.push({filename:this.download,url:this.href})}
    remove(){}
  }
  const button=new Element('button'),select=new Element('select'),message=new Element('p'),start=new Element('button'),download=new Element('button'),close=new Element('button');
  const dialog=new Element('dialog');dialog.querySelector=selector=>selector==='select'?select:{'#log-export-message':message,'#log-export-start':start,'#log-export-download':download,'#log-export-close':close}[selector];
  const context=vm.createContext({document:{getElementById:id=>id==='export-session-logs'?button:null,body:new Element('body'),createElement:tag=>tag==='dialog'?dialog:new Element(tag)},
    setTimeout(callback,delay){const id=++timerId;timers.set(id,{callback,delay});return id},clearTimeout:id=>timers.delete(id),
    URL:{createObjectURL:()=> 'blob:offline-log-zip',revokeObjectURL(){}},
    fetch:async(url,options)=>{
      calls.push({url,options});const value=url==='/api/status'?{connected:false}:url==='/api/log-sessions'?{sessions:[{key:'session-key',session_id:'session',vehicle_id:'vehicle',started_at_utc_ms:100000}]}:
        url==='/api/log-export'?exported:{zip:'offline'};
      return{ok:true,json:async()=>value,blob:async()=>({type:'application/zip'})};
    }});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../web/session_logs.js'),'utf8'),context);
  context.MineTeleopSessionLogs.install();
  return{button,dialog,message,start,download,close,select,calls,downloads,timers,exported};
}

test('actual shipped export dialog saves a complete log ZIP while prominently reporting incomplete or unknown recording',async()=>{
  for(const recording of [{included:false,evidence_status:'incomplete',complete:false,summary:{cameras:[{recording:{issue_code:'recording_admission_overflow'}}]}},undefined,{included:false,evidence_status:'unknown',complete:null}]){
    const fixture=dialogFixture({state:'ready',complete:true,logs_complete:true,complete_scope:'logs_only',filename:'session.zip',recording});
    await fixture.button.onclick();assert.equal(fixture.download.hidden,false);assert.match(fixture.message.textContent,/三端日志收集完成/);
    assert.match(fixture.message.textContent,recording&&recording.evidence_status==='incomplete'?/录像不完整/:/录像完整性未确认/);
    assert.match(fixture.message.textContent,/下一步/);assert.match(fixture.message.textContent,/不包含视频文件/);assert.doesNotMatch(fixture.message.textContent,/整会话录像完整[。；]/);
    await fixture.start.onclick();assert.equal(fixture.download.hidden,false);await fixture.download.onclick();
    assert.equal(fixture.downloads.length,1);assert.equal(fixture.downloads[0].filename,'session.zip');
    assert(fixture.calls.some(call=>call.url==='/api/log-export/download'));assert.equal(fixture.download.disabled,false);
  }
});

test('actual shipped export poll separates current-attempt finalization from unknown full-session history',async()=>{
  const exported={state:'collecting',vehicle_progress:{claimed:true,bytes_received:1048576}},fixture=dialogFixture(exported);
  await fixture.button.onclick();assert.equal(fixture.download.hidden,true);assert.match(fixture.message.textContent,/正在收集三端日志/);
  Object.assign(exported,{state:'ready',complete:true,logs_complete:true,filename:'partial-history.zip',recording:{included:false,evidence_status:'attempt_finalized',complete:null,attempt_complete:true}});
  const timer=Array.from(fixture.timers.values()).find(value=>value.delay===1500);assert(timer);await timer.callback();
  assert.equal(fixture.download.hidden,false);assert.match(fixture.message.textContent,/本次录像封装完成.*整会话录像完整性未确认/);
  await fixture.download.onclick();assert.equal(fixture.downloads[0].filename,'partial-history.zip');
});

test('recording gap overrides a contradictory successful export claim and old metadata never proves session completeness',()=>{
  const gap={included:false,evidence_status:'finalized',complete:true,summary:{completeness_scope:'full_session',session_history_known:true,incomplete:true,cameras:[{recording:{issue_code:'recording_admission_overflow'}}]}};
  assert.match(exportStatusMessage({state:'ready',complete:true,recording:gap}),/录像不完整/);
  assert.match(exportStatusMessage({state:'ready',complete:true,recording:{included:false,evidence_status:'finalized',complete:true}}),/录像完整性未确认/);
  assert.match(exportStatusMessage({state:'ready',complete:true}),/不包含视频文件/);
});

test('actual shipped export dialog reports incomplete evidence without asserting AU loss and preserves proven admission gaps',async()=>{
  for(const issue of ['recording_status_unconfirmed','recording_admission_overflow']){
    const recording={included:false,evidence_status:'incomplete',complete:false,summary:{incomplete:true,cameras:[{camera_id:'front',recording:{incomplete:true,issue_code:issue}}]}};
    const fixture=dialogFixture({state:'ready',logs_complete:true,complete:true,filename:'recording-evidence.zip',recording});
    await fixture.button.onclick();assert.equal(fixture.download.hidden,false);assert.match(fixture.message.textContent,/三端日志收集完成/);
    if(issue==='recording_status_unconfirmed'){
      assert.match(fixture.message.textContent,/证据不足.*完整性待确认/);assert.doesNotMatch(fixture.message.textContent,/缺口|缺录|无法补录|不能补回/);
    }else assert.match(fixture.message.textContent,/录像不完整.*已确认.*缺口无法补录/);
    assert.match(fixture.message.textContent,/下一步/);assert.match(fixture.message.textContent,/不包含视频文件/);
    await fixture.download.onclick();assert.equal(fixture.downloads[0].filename,'recording-evidence.zip');
  }
});

const deadline=setTimeout(()=>{console.error('unfinished session log tests: '+Array.from(unfinished).join(', '));process.exitCode=1},10000);
Promise.all(pending).then(()=>{clearTimeout(deadline);process.exitCode=failed?1:0;console.log(passed+' session log JavaScript tests passed')});
