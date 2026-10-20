const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const api = require('../web/control_transport.js');
function fixture({onPending} = {}) {
  let time = 0, current = {session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true};
  const requests = [], frames = [], boundaries = [], raw = [], statuses = [], traces = [];
  let reply = () => ({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{"native":"original bytes"}',
    frame:{seq:1,intent_seq:1,session_id:'s',sent_at_utc_ms:123,control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});
  const courier = api.createCourier({scope:()=>current,now:()=>time,onPending,
    post:async(path,body)=>{requests.push({path,body});return reply()},send:(frame,bytes)=>{assert.equal(typeof bytes,'string');frames.push(frame);raw.push(bytes);return true},onBoundary:()=>boundaries.push(true),onStatus:value=>statuses.push(value),onTrace:value=>traces.push(value)});
  return {courier,requests,frames,boundaries,raw,statuses,traces,setTime:value=>time=value,setScope:value=>current=value,
    setReply:value=>reply=value,scope:()=>current,now:()=>time};
}
const cases = [];
const test = (name, action) => cases.push({name,action});
test('forwards only the unchanged native frame and records a safety boundary',async()=>{
  const f=fixture();await f.courier.tick();
  assert.equal(f.requests.length,1);assert.equal(f.frames.length,1);assert.equal(f.boundaries.length,1);
  assert.equal(f.raw[0],'{"native":"original bytes"}');
  assert.equal(f.frames[0].sent_at_utc_ms,123);assert.equal(f.frames[0].seq,1);
  assert.equal(f.requests[0].body.source_control_session_generation,7);assert.equal(f.requests[0].body.ui_control_epoch,2);
  await f.courier.tick();assert.equal(f.frames.length,1,'same packet was resent');
  assert.equal(f.courier.negotiated(),true);assert.equal(f.courier.activePath(),'dc');
});
test('one in-flight request and an obsolete peer response cannot publish control',async()=>{
  const f=fixture();let resolve;
  f.setReply(()=>new Promise(done=>resolve=done));const pending=f.courier.tick();await f.courier.tick();
  assert.equal(f.requests.length,1);
  f.setScope({...f.scope(),attempt:'b'});
  resolve({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,
    frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});
  await pending;assert.equal(f.frames.length,0);assert.equal(f.boundaries.length,0);
});
test('late local delivery, closed DC and wrong frame scopes never get sent',async()=>{
  for(const kind of ['delay','closed','wrong-session','wrong-attempt','wss-frame','disabled']) {
    const f=fixture();f.setReply(()=>{
      const value={protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:20,frame_json:'{}',
        frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}};
      if(kind==='delay')f.setTime(21);
      if(kind==='closed')f.setScope({...f.scope(),ready:false});
      if(kind==='wrong-session')value.frame.session_id='old';
      if(kind==='wrong-attempt')value.frame.control_transport.media_attempt='old';
      if(kind==='wss-frame')value.frame.control_transport.path='wss';
      if(kind==='disabled')value.enabled=false;
      return value;
    });await f.courier.tick();assert.equal(f.frames.length,0,kind);
  }
});
test('a newer vehicle result arriving during a request stays queued',async()=>{
  const f=fixture();const result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq});
  assert.equal(f.courier.receive(result(1)),true);
  let resolve;f.setReply(()=>new Promise(done=>resolve=done));const pending=f.courier.tick();
  f.courier.receive(result(2));resolve({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'wss',revision:1},valid_for_ms:0});await pending;
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'wss',revision:1},valid_for_ms:0}));
  await f.courier.tick();assert.equal(f.requests[1].body.vehicle_result.result_seq,2);
  assert.equal(f.courier.activePath(),'wss');
  assert.equal(f.courier.receive({...result(3),session_id:'old'}),false);
});
test('unclaimed page and missing media scope cannot fetch native packets',async()=>{
  for(const patch of [{uiEpoch:0},{session:''},{attempt:''},{ui:''}]) {
    const f=fixture();f.setScope({...f.scope(),...patch});await f.courier.tick();assert.equal(f.requests.length,0);
  }
});
test('feedback diagnostics separate mailbox wait and HTTP delay without renewing a frame',async()=>{
  const f=fixture();
  const result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq});
  f.setTime(10);f.courier.receive(result(1));f.setTime(20);f.courier.receive(result(2));
  f.setTime(25);let resolve;f.setReply(()=>new Promise(done=>resolve=done));const pending=f.courier.tick();
  f.setTime(30);f.courier.receive(result(3));f.setTime(240);
  resolve({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{}',
    frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});await pending;
  const trace=f.traces.at(-1);assert.equal(trace.seq,1);assert.equal(trace.browser_mailbox_wait_ms,15);
  assert.equal(trace.bridge_http_elapsed_ms,215);assert.equal(trace.completed,true);assert.equal(f.frames.length,0);
  assert.equal(f.traces[1].superseded_result_seq,null);
  f.setReply(()=>({protocol:api.protocol,enabled:false}));await f.courier.tick();
  assert.equal(f.requests.at(-1).body.vehicle_result.result_seq,2);assert.equal(f.traces.at(-1).browser_mailbox_wait_ms,220);
  await f.courier.tick();assert.equal(f.requests.at(-1).body.vehicle_result.result_seq,3);
});
test('burst receipts preserve every result including a rejection before a newer acceptance',async()=>{
  const f=fixture(),result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq,accepted:seq!==2});
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1}}));
  for(let seq=1;seq<=12;seq++)assert.equal(f.courier.receive(result(seq)),true);
  assert.equal(f.courier.receive(result(2)),true,'duplicate reception remains recognized');
  for(let seq=1;seq<=12;seq++)await f.courier.tick();
  assert.deepEqual(f.requests.map(request=>request.body.vehicle_result.command_seq),Array.from({length:12},(_,i)=>i+1));
  assert.equal(f.requests[1].body.vehicle_result.accepted,false,'negative result was overwritten');
  await f.courier.tick();assert.equal(f.requests.at(-1).body.vehicle_result,undefined);
});
test('failed HTTP retains all queued receipts and scope change discards the old queue',async()=>{
  const f=fixture(),result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq});
  f.courier.receive(result(1));let reject;f.setReply(()=>new Promise((_,fail)=>reject=fail));
  const pending=f.courier.tick();f.courier.receive(result(2));f.courier.receive(result(3));reject(Error('fixture failure'));
  await assert.rejects(pending,/fixture failure/);
  f.setReply(()=>({protocol:api.protocol,enabled:false}));await f.courier.tick();await f.courier.tick();await f.courier.tick();
  assert.deepEqual(f.requests.map(request=>request.body.vehicle_result.command_seq),[1,1,2,3]);
  f.courier.receive(result(4));f.setScope({...f.scope(),attempt:'new'});await f.courier.tick();
  assert.equal(f.requests.at(-1).body.vehicle_result,undefined);
});
test('blocked bridge has a bounded receipt queue and reports overflow explicitly',async()=>{
  const f=fixture(),result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq});
  for(let seq=1;seq<=64;seq++)assert.equal(f.courier.receive(result(seq)),true);
  assert.equal(f.courier.receive(result(65)),false);
  assert.equal(f.traces.at(-1).stage,'dc_result_queue_overflow');assert.equal(f.traces.at(-1).seq,65);
  f.setReply(()=>({protocol:api.protocol,enabled:false}));
  for(let seq=1;seq<=64;seq++)await f.courier.tick();
  assert.deepEqual(f.requests.map(request=>request.body.vehicle_result.command_seq),Array.from({length:64},(_,i)=>i+1));
});
test('only a confirmed DataChannel send gets an actual-send trace and none is retried',async()=>{
  for(const outcome of [true,undefined,false,{reason:'channel_buffer_not_empty'},'throw']) {
    const f=fixture(),traces=[];let calls=0;
    const courier=api.createCourier({scope:f.scope,now:()=>0,onBoundary:()=>{},onTrace:value=>traces.push(value),
      post:async()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{}',
        frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}}),
      send:()=>{calls++;if(outcome==='throw')throw Error('fixture send');return outcome}});
    if(outcome==='throw')await assert.rejects(courier.tick(),/fixture send/);else await courier.tick();
    await courier.tick();assert.equal(calls,1);
    assert.equal(traces.at(-1).stage,outcome===true?'dc_command_sent':outcome==='throw'?'dc_command_send_failed':'dc_command_not_sent');
  }
});
test('trace failures cannot interrupt feedback or change control eligibility',async()=>{
  const current={session:'s',generation:1,ui:'owner',uiEpoch:1,attempt:'a',ready:true};let requests=0;
  const courier=api.createCourier({scope:()=>current,now:()=>0,send:()=>{},onBoundary:()=>{},
    onTrace:()=>{throw Error('diagnostic sink failed')},post:async(_url,body)=>{requests++;assert.equal(body.vehicle_result.command_seq,1);return {protocol:api.protocol,enabled:false}}});
  assert.equal(courier.receive({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:1,result_seq:1}),true);
  await courier.tick();assert.equal(requests,1);
});
const feedbackStamp=epoch=>({protocol:api.feedbackProtocol,enabled:true,epoch,from_epoch:epoch-1,path:'dc',media_attempt:'a',authority_issuer:'issuer',authority_epoch:7,status_floor:0});
const feedbackStatus=(epoch,seq,kind='vehicle_telemetry')=>({event:'vehicle_control_status',protocol:api.protocol,session_id:'s',feedback_transport:feedbackStamp(epoch),status:{event:kind,control_status_seq:seq}});
test('feedback epoch is compared before four-kind mailbox sequences and native-only status delivery',async()=>{
  const f=fixture();f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},feedback_transport:feedbackStamp(1)}));await f.courier.tick();
  assert.equal(f.courier.receive(feedbackStatus(1,Number.MAX_SAFE_INTEGER)),true);
  assert.equal(f.courier.receive(feedbackStatus(2,1)),true);
  assert.equal(f.courier.receive(feedbackStatus(99,2)),false);
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},feedback_transport:feedbackStamp(2),
    statuses:[{...feedbackStatus(1,Number.MAX_SAFE_INTEGER).status,feedback_transport:feedbackStamp(1)}, {...feedbackStatus(2,1).status,feedback_transport:feedbackStamp(2)}]}));
  await f.courier.tick();assert.equal(f.requests[1].body.vehicle_statuses.length,1);assert.equal(f.requests[1].body.vehicle_statuses[0].status.control_status_seq,1);
  assert.equal(f.statuses.length,1);assert.equal(f.statuses[0].control_status_seq,1);assert.equal(f.boundaries.length,1,'feedback-only switch cleared ordinary input');
  const result=(epoch,seq)=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq,feedback_transport:feedbackStamp(epoch)});
  assert.equal(f.courier.receive(result(1,Number.MAX_SAFE_INTEGER)),false);assert.equal(f.courier.receive(result(2,3)),true);
});
test('feedback snapshots arriving during an HTTP request survive and retired scope cannot leak them',async()=>{
  const f=fixture();f.courier.receive(feedbackStatus(1,1));let resolve;
  f.setReply(()=>new Promise(done=>resolve=done));const pending=f.courier.tick();f.courier.receive(feedbackStatus(1,2));
  resolve({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'wss',revision:1},feedback_transport:feedbackStamp(1)});await pending;
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'wss',revision:1},feedback_transport:feedbackStamp(1)}));await f.courier.tick();
  assert.equal(f.requests[1].body.vehicle_statuses[0].status.control_status_seq,2);
  f.setScope({...f.scope(),attempt:'new'});await f.courier.tick();assert.equal(f.requests[2].body.vehicle_statuses,undefined);
});
test('explicit scoped zero is forwarded only for the current pending selection, never as a reset authority',async()=>{
  const f=fixture();let state={...feedbackStamp(2),path:'wss',selecting:true};
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'wss',revision:1},feedback_transport:state}));await f.courier.tick();
  const result={event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:10,result_seq:10,
    feedback_request:{...state,select:true,probe:true},feedback_transport:{...state,epoch:0,from_epoch:0,state:'uninitialized'}};
  assert.equal(f.courier.receive(result),true);state={...feedbackStamp(1),path:'wss',selecting:true};await f.courier.tick();
  assert.equal(f.requests[1].body.vehicle_result.feedback_transport.epoch,0);assert.equal(f.courier.feedback().epoch,1);
  const missing=structuredClone(result);delete missing.feedback_transport.state;assert.equal(f.courier.receive(missing),false);
  state={...feedbackStamp(2),path:'wss',selecting:false};await f.courier.tick();assert.equal(f.courier.receive(result),false,'confirmed state accepted zero downgrade');
});
function emergencyFixture({onPending} = {}) {
  let time=0,current={session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true};
  const requests=[],sent=[],confirmed=[];
  const response=()=>({protocol:api.emergencyProtocol,enabled:true,valid_for_ms:100,frame_json:'{"native_stop":"original 0.0 bytes"}',
    software_latched_confirmed:false,physical_execution_confirmed:false,covers_all_commands:false,
    frame:{seq:1,session_id:'s',estop:true,throttle:0,steering:0,brake:1,emergency_stop:{protocol:api.emergencyProtocol,media_attempt:'a'}}});
  let reply=response;
  const courier=api.createEmergencyCourier({scope:()=>current,now:()=>time,onPending,post:async(path,body)=>{requests.push({path,body});return reply()},
    send:bytes=>sent.push(bytes),onConfirmation:seq=>confirmed.push(seq)});
  return{courier,requests,sent,confirmed,response,setReply:value=>reply=value,setTime:value=>time=value,setScope:value=>current=value,scope:()=>current,now:()=>time};
}
test('emergency courier operates while the ordinary courier request is blocked',async()=>{
  const ordinary=fixture(),stop=emergencyFixture();let complete;
  ordinary.setReply(()=>new Promise(resolve=>complete=resolve));const pending=ordinary.courier.tick();
  await stop.courier.tick();
  assert.equal(ordinary.frames.length,0);assert.equal(stop.sent.length,1);assert.equal(stop.sent[0],'{"native_stop":"original 0.0 bytes"}');
  assert.equal(stop.requests[0].path,'/api/control-transport/emergency');
  complete({protocol:api.protocol,enabled:false,transport:{epoch:0,path:'wss',revision:0}});await pending;
});
test('emergency raw bytes are single-send and normalized stop-only',async()=>{
  const f=emergencyFixture();await f.courier.tick();await f.courier.tick();assert.equal(f.sent.length,1);
  for(const field of ['estop','throttle','steering','brake']) {
    const bad=emergencyFixture();bad.setReply(()=>{const value=bad.response();value.frame[field]=field==='estop'?false:0.5;return value});
    await bad.courier.tick();assert.equal(bad.sent.length,0,field);
  }
});
test('emergency late transport and retired page response cannot publish a stale stop',async()=>{
  const late=emergencyFixture();late.setReply(()=>{late.setTime(101);return late.response()});
  await late.courier.tick();assert.equal(late.sent.length,0);late.setTime(102);await late.courier.tick();assert.equal(late.sent.length,0,'expired frame retried');
  const retired=emergencyFixture();let complete;
  retired.setReply(()=>new Promise(resolve=>complete=resolve));const pending=retired.courier.tick();await retired.courier.tick();
  assert.equal(retired.requests.length,1);retired.setScope({...retired.scope(),uiEpoch:3});complete(retired.response());await pending;
  assert.equal(retired.sent.length,0);
});
test('emergency confirmation is fresh software evidence, never physical or cumulative completion',async()=>{
  for(const patch of [{physical_execution_confirmed:true},{covers_all_commands:true},{enabled:false}]) {
    const f=emergencyFixture();f.setReply(()=>({...f.response(),software_latched_confirmed:true,confirmed_command_seq:1,...patch}));
    await f.courier.tick();assert.equal(f.confirmed.length,0);
  }
  const f=emergencyFixture();f.setReply(()=>({...f.response(),software_latched_confirmed:true,confirmed_command_seq:1}));
  await f.courier.tick();await f.courier.tick();assert.deepEqual(f.confirmed,[1]);
});
test('emergency result mailbox keeps advancing current-session results during HTTP',async()=>{
  const f=emergencyFixture();const result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,emergency_stop:{protocol:api.emergencyProtocol}});
  assert.equal(f.courier.receive(result(1)),true);let complete;
  f.setReply(()=>new Promise(resolve=>complete=resolve));const pending=f.courier.tick();f.courier.receive(result(2));complete(f.response());await pending;
  f.setReply(f.response);await f.courier.tick();assert.equal(f.requests[1].body.vehicle_result.command_seq,2);
  assert.equal(f.courier.receive({...result(3),session_id:'retired'}),false);
});
// Extract the actual shipped callback and threshold, so a fixture cannot silently
// replace the production 1024-byte low watermark with the old review's zero.
function emergencyBufferFixture(overrideSend) {
  const source=fs.readFileSync(path.join(__dirname,'../src/server.cpp'),'utf8');
  const section=source.slice(source.indexOf('const emergencyStopCourier='),source.indexOf('const emergencyStopScheduler='));
  const prefix='post,now:()=>performance.now(),send:bytes=>';
  const start=section.indexOf(prefix);assert.ok(start>=0,'shipped emergency callback missing');
  const body=section.slice(start+prefix.length,section.indexOf('\n  onConfirmation:',start)).trim().replace(/,$/,'');
  const threshold=Number(source.match(/channel\.bufferedAmountLowThreshold=(\d+);/)[1]);assert.equal(threshold,1024);
  let time=0,current={session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true},publication,replyHook=null,enabled=true;
  const nativeScope={...current},requests=[],sent=[];
  const channel={readyState:'open',bufferedAmount:threshold+1,bufferedAmountLowThreshold:threshold,send:bytes=>sent.push(bytes)};
  const send=overrideSend||new Function('controlChannel','MineTeleopControlTransport',`return bytes=>${body}`)(channel,api);
  function publish(seq=1) {
    const frame={seq,intent_seq:seq*7,session_id:'s',sent_at_utc_ms:123+time,estop:true,throttle:0,steering:0,brake:1,
      native_mac:`immutable-fixture-mac-${seq}`,emergency_stop:{protocol:api.emergencyProtocol,media_attempt:'a',reset_epoch:0,authority:{issuer:'fixture',authority_epoch:4,lease_seq:2}}};
    publication={frame,bytes:JSON.stringify(frame),published:time};return publication;
  }
  publish();
  const courier=api.createEmergencyCourier({scope:()=>current,now:()=>time,send,post:async(_url,request)=>{
    requests.push(request);
    const scoped=request.source_session_id===nativeScope.session&&request.source_control_session_generation===nativeScope.generation&&
      request.ui_instance_id===nativeScope.ui&&request.ui_control_epoch===nativeScope.uiEpoch&&request.media_negotiation_id===nativeScope.attempt;
    const age=time-publication.published;
    const available=enabled&&scoped&&request.dc_ready&&age>=0&&age<100&&publication.frame.seq>request.last_seen_seq;
    const response={protocol:api.emergencyProtocol,enabled,valid_for_ms:available?100-age:0,
      frame:available?publication.frame:null,frame_json:available?publication.bytes:null};
    return replyHook?replyHook(response):response;
  }});
  return {courier,channel,requests,sent,publish,scope:()=>current,setScope:value=>current=value,setTime:value=>time=value,
    setReplyHook:value=>replyHook=value,setEnabled:value=>enabled=value,publication:()=>publication};
}
test('R5 actual 1025-byte backpressure preserves the same unqueued STOP until recovery, then consumes once',async()=>{
  const f=emergencyBufferFixture(),original=f.publication();await f.courier.tick();assert.equal(f.sent.length,0);
  f.setTime(25);f.channel.bufferedAmount=0;await f.courier.tick();
  assert.equal(f.requests[1].last_seen_seq,0,'COUNTEREXAMPLE R5: skipped send consumed the still-valid original STOP');
  assert.deepEqual(f.sent,[original.bytes]);assert.equal(f.publication(),original,'courier regenerated publication');
  f.setTime(50);await f.courier.tick();assert.equal(f.requests[2].last_seen_seq,1);assert.equal(f.sent.length,1,'missing ACK caused an enqueued STOP retry');
});
test('R5 actual 1024-byte threshold sends once and unknown outcomes or throws never qualify for retry',async()=>{
  const ready=emergencyBufferFixture();ready.channel.bufferedAmount=1024;await ready.courier.tick();
  ready.setTime(25);await ready.courier.tick();assert.equal(ready.sent.length,1);
  for(const outcome of [undefined,false,0,'backpressure',{reason:'buffered_amount_above_threshold'},Promise.resolve(api.emergencyBackpressure)]) {
    let calls=0;const f=emergencyBufferFixture(()=>{calls++;return outcome});f.channel.bufferedAmount=0;
    await f.courier.tick();f.setTime(25);await f.courier.tick();assert.equal(calls,1,'unknown callback result retried');
    assert.equal(f.requests[1].last_seen_seq,1);
  }
  const thrown=emergencyBufferFixture();thrown.channel.bufferedAmount=0;let calls=0;
  thrown.channel.send=()=>{calls++;throw new Error('fixture send failed')};
  await assert.rejects(thrown.courier.tick(),/fixture send failed/);thrown.setTime(25);await thrown.courier.tick();
  assert.equal(calls,1,'throwing send retried');assert.equal(thrown.requests[1].last_seen_seq,1);
});
test('R5 recovery never extends the original 100ms publication or reuses an expired reply',async()=>{
  const f=emergencyBufferFixture(),original=f.publication();await f.courier.tick();
  f.setTime(100);f.channel.bufferedAmount=0;await f.courier.tick();assert.equal(f.requests[1].last_seen_seq,0);
  assert.equal(f.sent.length,0);assert.equal(f.publication(),original);
  const delayed=emergencyBufferFixture();await delayed.courier.tick();delayed.setTime(99);delayed.channel.bufferedAmount=0;
  delayed.setReplyHook(response=>{assert.equal(response.valid_for_ms,1);delayed.setTime(100);return response});
  await delayed.courier.tick();assert.equal(delayed.sent.length,0,'HTTP delay renewed original publication');
});
test('R5 a newer native STOP replaces the blocked frame without catch-up or resigning',async()=>{
  const f=emergencyBufferFixture(),old=f.publication();await f.courier.tick();f.setTime(10);const latest=f.publish(2);
  f.setTime(25);f.channel.bufferedAmount=0;await f.courier.tick();
  assert.deepEqual(f.sent,[latest.bytes]);assert.notEqual(latest.bytes,old.bytes);assert.equal(f.publication(),latest);
  f.setTime(50);await f.courier.tick();assert.equal(f.requests[2].last_seen_seq,2);assert.equal(f.sent.length,1);
});
test('R5 a blocked STOP cannot cross current session generation page or media scope, including a late HTTP reply',async()=>{
  for(const patch of [{session:'other'},{generation:8},{ui:'replacement'},{uiEpoch:3},{attempt:'new'}]) {
    const f=emergencyBufferFixture();await f.courier.tick();f.setTime(25);f.channel.bufferedAmount=0;
    let complete;f.setReplyHook(response=>new Promise(resolve=>complete=()=>resolve(response)));
    const pending=f.courier.tick();await f.courier.tick();assert.equal(f.requests.length,2,'overlapping HTTP fetch');
    f.setScope({...f.scope(),...patch});complete();await pending;assert.equal(f.sent.length,0);
    f.setReplyHook(null);await f.courier.tick();assert.equal(f.sent.length,0,'old native scope leaked after replacement');
  }
  const disabled=emergencyBufferFixture();await disabled.courier.tick();disabled.setTime(25);disabled.channel.bufferedAmount=0;
  disabled.setEnabled(false);await disabled.courier.tick();assert.equal(disabled.sent.length,0,'cached frame bypassed native eligibility');
});
test('R5 a closed channel or invalid buffer evidence is terminal rather than guessed backpressure',async()=>{
  for(const patch of [{readyState:'closed'},{bufferedAmount:NaN},{bufferedAmount:Infinity},{bufferedAmount:-1},{bufferedAmountLowThreshold:NaN}]) {
    const f=emergencyBufferFixture();Object.assign(f.channel,{bufferedAmount:0,...patch});await f.courier.tick();assert.equal(f.sent.length,0);
    Object.assign(f.channel,{readyState:'open',bufferedAmount:0,bufferedAmountLowThreshold:1024});f.setTime(25);await f.courier.tick();
    assert.equal(f.requests[1].last_seen_seq,1);assert.equal(f.sent.length,0,'unknown channel state retried');
  }
});
test('product reads the SCTP selected candidate pair and never media statistics',async()=>{
  const channel={readyState:'open'};
  let pair={local:{type:'host'},remote:{type:'srflx'}};
  const ice={state:'connected',getSelectedCandidatePair:()=>pair};
  const peer={connectionState:'connected',sctp:{state:'connected',transport:{state:'connected',iceTransport:ice}}};
  assert.deepEqual(api.dataChannelRoute(peer,channel),{networkPath:'p2p',ready:true});
  for(const type of ['host','srflx','prflx']) {
    pair={local:{type},remote:{type:'prflx'}};
    assert.equal(api.dataChannelRoute(peer,channel).ready,true);
  }
  for(const side of ['local','remote']) {
    pair={local:{type:'host'},remote:{type:'srflx'}};pair[side].type='relay';
    assert.deepEqual(api.dataChannelRoute(peer,channel),{networkPath:'turn_unknown',ready:false});
  }
  for(const invalid of [null,{}, {local:{type:'host'}}, {local:{type:'host'},remote:{type:'unknown'}}]) {
    pair=invalid;assert.deepEqual(api.dataChannelRoute(peer,channel),{networkPath:'unknown',ready:false});
  }
  ice.getSelectedCandidatePair=()=>{throw Error('unsupported')};
  assert.equal(api.dataChannelRoute(peer,channel).ready,false);
  assert.equal(api.dataChannelRoute({connectionState:'connected'},channel).ready,false);
  ice.getSelectedCandidatePair=()=>({local:{type:'host'},remote:{type:'host'}});
  assert.equal(api.dataChannelRoute(peer,channel,'cloud_only').ready,false);
  for(const target of [peer,peer.sctp,peer.sctp.transport,ice,channel]) {
    const key=target===peer?'connectionState':target===channel?'readyState':'state',old=target[key];
    target[key]='disconnected';assert.equal(api.dataChannelRoute(peer,channel).ready,false);target[key]=old;
  }
});
test('a P2P to TURN switch while fetching discards native bytes without replay',async()=>{
  const f=fixture();f.setScope({...f.scope(),networkPath:'p2p',ready:true});
  let resolve;f.setReply(()=>new Promise(done=>resolve=done));const pending=f.courier.tick();
  f.setScope({...f.scope(),networkPath:'turn_unknown',ready:false});
  resolve({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{}',
    frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});
  await pending;assert.equal(f.frames.length,0);
  f.setScope({...f.scope(),networkPath:'p2p',ready:true});
  f.setReply(()=>({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{}',
    frame:{seq:1,session_id:'s',control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}}));
  await f.courier.tick();assert.equal(f.frames.length,0,'discarded pre-switch packet replayed');
  assert.equal(f.requests[1].body.last_seen_seq,1);
});
function wakeFixture(emergency=false) {
  let scheduler;
  const callbacks=[],errors=[];
  const f=(emergency?emergencyFixture:fixture)({onPending:guard=>scheduler.request(guard)});
  scheduler=api.createTickScheduler({tick:()=>f.courier.tick(),beforeTick:()=>f.courier.cancelStaleRequest(),now:f.now,
    schedule:callback=>callbacks.push(callback),onError:error=>errors.push(error.message)});
  const result=seq=>({event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:seq,result_seq:seq,
    accepted:seq!==2,...(emergency?{emergency_stop:{protocol:api.emergencyProtocol}}:{})});
  async function flush() {
    let count=0;
    while(callbacks.length){assert.ok(++count<=70,'unbounded notification loop');await callbacks.shift()()}
  }
  return {...f,scheduler,callbacks,errors,result,flush};
}
test('receipt wakeups drain FIFO rejections and latest status without timer or duplicate command sends',async()=>{
  const f=wakeFixture();
  for(let seq=1;seq<=12;seq++)f.courier.receive(f.result(seq));
  f.courier.receive(feedbackStatus(1,1));f.courier.receive(feedbackStatus(1,2));
  for(let i=0;i<20;i++)f.courier.receive(f.result(2));
  assert.equal(f.requests.length,0);assert.equal(f.callbacks.length,1,'burst did not coalesce');
  await f.flush();
  assert.deepEqual(f.requests.map(r=>r.body.vehicle_result.command_seq),Array.from({length:12},(_,i)=>i+1));
  assert.equal(f.requests[1].body.vehicle_result.accepted,false);
  assert.equal(f.requests[0].body.vehicle_statuses[0].status.control_status_seq,2);
  assert.equal(f.frames.length,1,'feedback wakeup replayed the same native command');
  assert.ok(f.traces.filter(r=>r.stage==='dc_result_bridge_completed').every(r=>r.browser_mailbox_wait_ms===0));
  assert.equal(f.callbacks.length,0);assert.deepEqual(f.errors,[]);
});
test('receipts during HTTP cause one follow-up and emergency remains independent',async()=>{
  for(const emergency of [false,true]) {
    const f=wakeFixture(emergency);let complete;
    f.setReply(()=>new Promise(resolve=>complete=resolve));
    f.courier.receive(f.result(1));const pending=f.callbacks.shift()();
    f.courier.receive(f.result(2));f.courier.receive(f.result(3));
    await f.scheduler.tick();assert.equal(f.requests.length,1);assert.equal(f.callbacks.length,0);
    if(!emergency){const stop=wakeFixture(true);stop.courier.receive(stop.result(1));await stop.flush();assert.equal(stop.sent.length,1)}
    const response=emergency?f.response():{protocol:api.protocol,enabled:false};
    f.setReply(()=>response);complete(response);await pending;
    assert.equal(f.callbacks.length,1);await f.flush();
    assert.deepEqual(f.requests.map(r=>r.body.vehicle_result.command_seq),emergency?[1,3]:[1,2,3]);
    if(emergency)assert.equal(f.sent.length,1,'emergency feedback resent a queued STOP');
  }
});
test('HTTP failure cannot be accelerated by receipt bursts and keeps data for bounded retry',async()=>{
  for(const emergency of [false,true]) {
    const f=wakeFixture(emergency);let reject;
    f.setReply(()=>new Promise((_,fail)=>reject=fail));
    f.courier.receive(f.result(1));const pending=f.callbacks.shift()();
    f.courier.receive(f.result(2));reject(Error('offline'));await pending;
    assert.equal(f.callbacks.length,0);assert.deepEqual(f.errors,['offline']);
    for(let i=0;i<20;i++)f.courier.receive(f.result(3));
    await f.scheduler.tick();f.setTime(24);await f.scheduler.tick();
    assert.equal(f.callbacks.length,0);assert.equal(f.requests.length,1);
    f.setReply(emergency?f.response:()=>({protocol:api.protocol,enabled:false}));
    f.setTime(25);await f.scheduler.tick();await f.flush();
    assert.deepEqual(f.requests.map(r=>r.body.vehicle_result.command_seq),emergency?[1,3]:[1,1,2,3]);
  }
});
test('retired queued notifications and in-flight responses cannot wake a replacement scope',async()=>{
  for(const emergency of [false,true])for(const inFlight of [false,true])for(const patch of [
    {session:'new'},{generation:8},{ui:'new'},{uiEpoch:3},{attempt:'new'}]) {
    const f=wakeFixture(emergency);let complete,pending;
    f.courier.receive(f.result(1));
    if(inFlight){f.setReply(()=>new Promise(resolve=>complete=resolve));pending=f.callbacks.shift()();f.courier.receive(f.result(2))}
    f.setScope({...f.scope(),...patch});
    if(inFlight){complete(emergency?f.response():{protocol:api.protocol,enabled:false});await pending}
    await f.flush();assert.equal(f.requests.length,inFlight?1:0);
    assert.equal((emergency?f.sent:f.frames).length,0,'retired response sent bytes');
    f.setReply(()=>({protocol:emergency?api.emergencyProtocol:api.protocol,enabled:false}));
    await f.scheduler.tick();assert.equal(f.requests.at(-1).body.vehicle_result,undefined,'old feedback leaked into periodic fetch');
  }
});
test('notification and periodic tick racing use one owner and ignore invalid protocol wake loops',async()=>{
  const f=wakeFixture();f.courier.receive(f.result(1));
  await f.scheduler.tick();await f.flush();assert.equal(f.requests.length,1,'queued wake repeated the periodic fetch');
  f.setReply(()=>({protocol:'invalid'}));f.courier.receive(f.result(2));await f.flush();
  assert.equal(f.requests.length,2);assert.equal(f.callbacks.length,0,'invalid response caused automatic retry loop');
  assert.equal(f.courier.receive({...f.result(3),session_id:'retired'}),false);assert.equal(f.callbacks.length,0);
  f.setReply(()=>({protocol:api.protocol,enabled:false}));await f.scheduler.tick();
  assert.equal(f.requests.at(-1).body.vehicle_result.command_seq,2);
});
test('shipped page wires both result notifications to the scheduler and retains periodic fetches',async()=>{
  const source=fs.readFileSync(path.join(__dirname,'../src/server.cpp'),'utf8');
  const first=source.indexOf('const controlTransportCourier='),last=source.indexOf('setInterval(()=>cameraView.refresh(),500);',first);
  assert.ok(first>=0&&last>first);
  const requests=[],timers=[];
  const context={MineTeleopControlTransport:api,performance:{now:()=>0},polling:true,controlAuthorityLost:false,
    nativeControlSessionId:'s',nativeControlSessionGeneration:7,uiInstanceId:'owner',nativeControlUiEpoch:2,mediaOfferScope:{id:'a'},
    latestRuntimeStatus:{native_control:{control_transport:{enabled:true,mode:'cloud_only'},emergency_stop_capability:api.emergencyProtocol}},
    peer:null,controlChannel:null,controlTraceEnabled:false,applyVehicleControlMessage:()=>{},clearControlInput:()=>{},clientLog:()=>{},
    post:(...args)=>context.reply(...args),reply:async(url,body)=>{requests.push({url,body});return {protocol:url.endsWith('/emergency')?api.emergencyProtocol:api.protocol,enabled:false}},
    setInterval:(callback,ms)=>timers.push({callback,ms})};
  require('node:vm').runInNewContext(source.slice(first,last)+'\nglobalThis.receivers=[controlTransportCourier,emergencyStopCourier];',context);
  assert.equal(requests.length,0);assert.equal(timers.length,2);assert.ok(timers.every(t=>t.ms===25));
  const result={event:'vehicle_control_result',protocol:api.protocol,session_id:'s',command_seq:1,result_seq:1};
  context.receivers[0].receive(result);
  context.receivers[1].receive({...result,emergency_stop:{protocol:api.emergencyProtocol}});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(requests.length,2,'actual page waited for a periodic tick');
  assert.ok(requests.every(r=>r.body.vehicle_result.command_seq===1));
  for(const timer of timers)await timer.callback();
  assert.equal(requests.length,4,'native publication fallback was removed');
  const signals=[];context.reply=(_,__,signal)=>{signals.push(signal);return new Promise(()=>{})};
  const inFlight=timers.map(timer=>timer.callback());
  context.mediaOfferScope={id:'replacement'};
  for(const timer of timers)await timer.callback();
  assert.equal(signals.length,2);assert.ok(signals.every(signal=>signal.aborted),'shipped scheduler did not cancel both lanes');
  await Promise.all(inFlight);
});
test('both couriers abort a stalled HTTP request and can fetch again without replaying its reply',async()=>{
  for(const create of [api.createCourier,api.createEmergencyCourier]) {
    let calls=0,resolveLate,signal,sends=0;
    const scope=()=>({session:'s',generation:1,ui:'u',uiEpoch:1,attempt:'a',ready:true});
    const courier=create({scope,now:()=>0,requestTimeoutMs:10,onBoundary:()=>{},send:()=>{sends++},
      post:(_,__,nextSignal)=>{signal=nextSignal;calls++;return calls===1?new Promise(resolve=>resolveLate=resolve):Promise.resolve({enabled:false})}});
    const first=courier.tick();await courier.tick();assert.equal(calls,1);
    await assert.rejects(first,/timed out/);assert.equal(signal.aborted,true);
    await courier.tick();assert.equal(calls,2);
    resolveLate({enabled:true,frame:{seq:99}});await Promise.resolve();
    assert.equal(sends,0,'obsolete HTTP reply sent a command');
  }
});
test('both couriers cancel a pending request when the session scope changes',async()=>{
  for(const create of [api.createCourier,api.createEmergencyCourier]) {
    let current={session:'s',generation:1,ui:'u',uiEpoch:1,attempt:'a',ready:true},calls=0,signal;
    const courier=create({scope:()=>current,now:()=>0,onBoundary:()=>{},send:()=>assert.fail('obsolete frame'),
      post:(_,__,nextSignal)=>{signal=nextSignal;calls++;return calls===1?new Promise(()=>{}):Promise.resolve({enabled:false})}});
    const pending=assert.rejects(courier.tick(),/cancelled/);
    current={...current,attempt:'new'};await courier.tick();await pending;
    assert.equal(signal.aborted,true);await courier.tick();assert.equal(calls,2);
  }
});
test('production scheduler cancels stale in-flight requests in both lanes before timeout',async()=>{
  for(const create of [api.createCourier,api.createEmergencyCourier])for(const patch of [
    {session:'new'},{generation:2},{ui:'new'},{uiEpoch:2},{attempt:'new'},null]) {
    let current={session:'s',generation:1,ui:'u',uiEpoch:1,attempt:'a',ready:true},calls=0,signal,resolveLate,time=0;
    const courier=create({scope:()=>current,now:()=>time,onBoundary:()=>{},send:()=>assert.fail('retired frame sent'),
      post:(_,__,nextSignal)=>{calls++;if(calls===1){signal=nextSignal;return new Promise(resolve=>resolveLate=resolve)}return Promise.resolve({enabled:false})}});
    const scheduler=api.createTickScheduler({tick:()=>courier.tick(),beforeTick:()=>courier.cancelStaleRequest(),now:()=>time,onError:()=>{}});
    const pending=scheduler.tick();await scheduler.tick();assert.equal(calls,1);
    current=patch?{...current,...patch}:null;
    await scheduler.tick();assert.equal(signal.aborted,true,'scheduler busy guard hid scope cancellation');
    await pending;assert.equal(calls,1,'scheduler overlapped requests');
    time=25;await scheduler.tick();assert.equal(calls,patch?2:1,'replacement scope stayed blocked for 500ms');
    resolveLate({enabled:true,frame:{seq:99}});await Promise.resolve();
  }
});
(async()=>{
  const selected=process.argv.includes('--r5-only')?cases.filter(value=>value.name.startsWith('R5 ')):cases;
  for(const {name,action} of selected){await action();console.log('PASS '+name)}
  console.log(`${selected.length} transport courier cases passed; no sockets`);
})().catch(error=>{console.error(error);process.exitCode=1});
