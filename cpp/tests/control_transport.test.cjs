const assert = require('node:assert/strict');
const api = require('../web/control_transport.js');
function fixture() {
  let time = 0, current = {session:'s',generation:7,ui:'owner',uiEpoch:2,attempt:'a',ready:true};
  const requests = [], frames = [], boundaries = [], raw = [];
  let reply = () => ({protocol:api.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:1},valid_for_ms:100,frame_json:'{"native":"original bytes"}',
    frame:{seq:1,intent_seq:1,session_id:'s',sent_at_utc_ms:123,control_transport:{protocol:api.protocol,path:'dc',media_attempt:'a'}}});
  const courier = api.createCourier({scope:()=>current,now:()=>time,
    post:async(path,body)=>{requests.push({path,body});return reply()},send:(frame,bytes)=>{assert.equal(typeof bytes,'string');frames.push(frame);raw.push(bytes)},onBoundary:()=>boundaries.push(true)});
  return {courier,requests,frames,boundaries,raw,setTime:value=>time=value,setScope:value=>current=value,
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
(async()=>{
  for(const {name,action} of cases){await action();console.log('PASS '+name)}
  console.log(`${cases.length} transport courier cases passed; no sockets`);
})().catch(error=>{console.error(error);process.exitCode=1});
