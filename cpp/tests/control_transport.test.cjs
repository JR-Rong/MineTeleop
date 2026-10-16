const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const api = require('../web/control_transport.js');
function fixture() {
  let time = 0, current = {session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true};
  const requests = [], frames = [], boundaries = [], raw = [], statuses = [];
  let reply = () => ({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{"native":"original bytes"}',
    frame:{seq:1,intent_seq:1,session_id:'s',sent_at_utc_ms:123,control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});
  const courier = api.createCourier({scope:()=>current,now:()=>time,
    post:async(path,body)=>{requests.push({path,body});return reply()},send:(frame,bytes)=>{assert.equal(typeof bytes,'string');frames.push(frame);raw.push(bytes)},onBoundary:()=>boundaries.push(true),onStatus:value=>statuses.push(value)});
  return {courier,requests,frames,boundaries,raw,statuses,setTime:value=>time=value,setScope:value=>current=value,
    setReply:value=>reply=value,scope:()=>current};
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
test('a newer vehicle result arriving during a request stays in the one-slot mailbox',async()=>{
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
function emergencyFixture() {
  let time=0,current={session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true};
  const requests=[],sent=[],confirmed=[];
  const response=()=>({protocol:api.emergencyProtocol,enabled:true,valid_for_ms:100,frame_json:'{"native_stop":"original 0.0 bytes"}',
    software_latched_confirmed:false,physical_execution_confirmed:false,covers_all_commands:false,
    frame:{seq:1,session_id:'s',estop:true,throttle:0,steering:0,brake:1,emergency_stop:{protocol:api.emergencyProtocol,media_attempt:'a'}}});
  let reply=response;
  const courier=api.createEmergencyCourier({scope:()=>current,now:()=>time,post:async(path,body)=>{requests.push({path,body});return reply()},
    send:bytes=>sent.push(bytes),onConfirmation:seq=>confirmed.push(seq)});
  return{courier,requests,sent,confirmed,response,setReply:value=>reply=value,setTime:value=>time=value,setScope:value=>current=value,scope:()=>current};
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
  const section=source.slice(source.indexOf('const emergencyStopCourier='),source.indexOf('setInterval(()=>emergencyStopCourier.tick()'));
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
(async()=>{
  const selected=process.argv.includes('--r5-only')?cases.filter(value=>value.name.startsWith('R5 ')):cases;
  for(const {name,action} of selected){await action();console.log('PASS '+name)}
  console.log(`${selected.length} transport courier cases passed; no sockets`);
})().catch(error=>{console.error(error);process.exitCode=1});
