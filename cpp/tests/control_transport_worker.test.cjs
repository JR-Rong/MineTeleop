'use strict';
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
const transport=require('../web/control_transport.js');
const adapter=require('../web/control_transport_worker.js');
const tests=[];const test=(name,run)=>tests.push({name,run});
const settle=async()=>{for(let i=0;i<10;i++)await Promise.resolve()};
function fixture() {
  let time=0, reply, pending;
  const timers=[],messages=[],requests=[],sent=[];
  const channel={readyState:'open',bufferedAmount:0,send:bytes=>sent.push(bytes),close(){this.readyState='closed';this.onclose?.()}};
  const self={location:{origin:'http://localhost:18872'},origin:'http://localhost:18872',postMessage:message=>messages.push(message)};
  const context={self,URL,performance:{timeOrigin:100000,now:()=>time},Date,AbortController,setTimeout,clearTimeout,queueMicrotask,
    setInterval:(action,ms)=>timers.push({action,ms}),fetch:async(url,options)=>{
      requests.push({url,body:JSON.parse(options.body),options});
      const value=reply?await reply(url):null;
      return {ok:true,json:async()=>value};
    }};
  vm.runInNewContext('('+adapter.workerMain.toString()+')('+transport.factorySource()+')',context);
  self.onmessage({data:{type:'init',owner:1,origin:self.origin,channel}});
  const scope={session:'s',generation:1,ui:'u',uiEpoch:1,attempt:'a',ready:true,networkPath:'p2p'};
  const publish=(patch={})=>self.onmessage({data:{type:'scope',owner:1,snapshot:{published:100000+time,control:{...scope,...patch},emergency:null}}});
  const frame=seq=>({protocol:transport.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:0},valid_for_ms:100,frame_json:'UNCHANGED_SIGNED_'+seq,
    frame:{seq,session_id:'s',control_transport:{protocol:transport.protocol,path:'dc',media_attempt:'a'}}});
  publish();
  return {self,channel,requests,messages,sent,publish,frame,setTime:value=>time=value,
    reply:value=>reply=value,tick:async()=>{timers.find(t=>t.ms===25).action();await settle()},
    drain:async()=>{timers.find(t=>t.ms===5).action();await settle()},
    view:()=>timers.find(t=>t.ms===100).action(),hold:()=>{reply=()=>new Promise(resolve=>pending=resolve);return value=>pending(value)}};
}
test('worker reuses the production factory and forwards exact bytes without generating input',async()=>{
  const f=fixture();f.reply(()=>f.frame(1));await f.tick();
  assert.deepEqual(f.sent,['UNCHANGED_SIGNED_1']);
  assert.equal(f.requests.length,1);assert.equal(f.requests[0].url,'http://localhost:18872/api/control-transport/bridge');
  assert.equal(f.requests[0].body.dc_ready,true);assert.equal(f.requests[0].options.credentials,'same-origin');
  f.view();assert.ok(f.messages.some(m=>m.type==='trace'&&m.records[0].execution_context==='dedicated_worker'));
  assert.ok(f.requests.every(r=>!r.url.includes('/intent')),'courier must never renew the input lease');
});
test('page publication expiry and delayed HTTP cannot send a cached motion frame',async()=>{
  for(const age of [200,201,-1001]) {
    const f=fixture(),resolve=f.hold();await f.tick();f.setTime(age);resolve(f.frame(1));await settle();
    assert.equal(f.sent.length,0,'invalid page scope age '+age);
  }
  const f=fixture();f.setTime(200);await f.tick();assert.equal(f.requests.length,0);
});
test('a snapshot a few milliseconds ahead of the worker clock stays valid',async()=>{
  // The page publishes from its own performance clock. Ordering jitter of a few
  // milliseconds must not read as an expired page: in the A50 probe that false
  // verdict cancelled 1.75% of all scope reads and every in-flight bridge request
  // behind them.
  for(const skew of [1,5,50]) {
    const f=fixture();f.setTime(skew);f.publish();f.setTime(0);f.reply(()=>f.frame(1));await f.tick();
    assert.deepEqual(f.sent,['UNCHANGED_SIGNED_1'],'future skew '+skew+' was treated as stale');
    assert.equal(f.requests.length,1,'future skew '+skew+' stayed blocked');
  }
});
test('retired session, UI epoch and media attempt discard late worker replies',async()=>{
  for(const patch of [{session:'new'},{uiEpoch:2},{attempt:'b'},{generation:2},{ui:'replacement'}]) {
    const f=fixture(),resolve=f.hold();await f.tick();f.publish(patch);resolve(f.frame(1));await settle();assert.equal(f.sent.length,0);
  }
});
test('worker keeps native deadline, actual channel readiness and backpressure checks',async()=>{
  for(const kind of ['deadline','closed','buffered','wrong-frame']) {
    const f=fixture();f.reply(()=>{const r=f.frame(1);if(kind==='deadline')f.setTime(100);if(kind==='closed')f.channel.readyState='closed';
      if(kind==='buffered')f.channel.bufferedAmount=1;if(kind==='wrong-frame')r.frame.control_transport.media_attempt='other';return r});
    await f.tick();assert.equal(f.sent.length,0,kind);
  }
});
test('pre-existing SCTP buffer withholds frame consumption without dropping dc_ready and sends on drain',async()=>{
  const f=fixture();
  f.channel.bufferedAmount=1200;
  let heldFrame=f.frame(7);
  f.reply(()=>f.requests.at(-1).body.dc_send_ready===false?{protocol:transport.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:0},frame:null}:heldFrame);
  await f.tick();
  assert.equal(f.requests.length,1);
  assert.equal(f.requests[0].body.dc_ready,true);
  assert.equal(f.requests[0].body.dc_send_ready,false);
  assert.equal(f.sent.length,0);
  // Drain SCTP buffer and fire onbufferedamountlow: courier wakes without waiting for 25ms timer and sends fresh frame 7.
  f.channel.bufferedAmount=0;
  f.channel.onbufferedamountlow();
  await settle();
  assert.equal(f.requests.length,2);
  assert.equal(f.requests[1].body.dc_send_ready,undefined);
  assert.equal(f.requests[1].body.last_seen_seq,0);
  assert.deepEqual(f.sent,['UNCHANGED_SIGNED_7']);
  // Sub-1024B buffer (single ~520B control frame): WebRTC onbufferedamountlow does not cross 1024B,
  // so 5ms local drain check stays silent while buffered>0 and wakes once when buffered hits 0.
  const subThreshold=fixture();
  subThreshold.channel.bufferedAmount=520;
  subThreshold.reply(()=>subThreshold.requests.at(-1).body.dc_send_ready===false?{protocol:transport.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:0},frame:null}:subThreshold.frame(9));
  await subThreshold.tick();
  assert.equal(subThreshold.requests.length,1);
  assert.equal(subThreshold.requests[0].body.dc_send_ready,false);
  await subThreshold.drain();
  assert.equal(subThreshold.requests.length,1,'drain check polled HTTP while buffer was still non-empty');
  subThreshold.channel.bufferedAmount=0;
  await subThreshold.drain();
  assert.equal(subThreshold.requests.length,2);
  assert.deepEqual(subThreshold.sent,['UNCHANGED_SIGNED_9']);
  await subThreshold.drain();
  assert.equal(subThreshold.requests.length,2,'drain check fired again after waitingForDrain cleared');
  // Negative check: if the frame expired before drain, onbufferedamountlow still refuses to send it.
  const expired=fixture();
  expired.channel.bufferedAmount=1200;
  expired.reply(()=>expired.requests.at(-1).body.dc_send_ready===false?{protocol:transport.protocol,enabled:true,transport:{epoch:1,path:'dc',revision:0},frame:null}:{...expired.frame(8),valid_for_ms:0});
  await expired.tick();
  expired.channel.bufferedAmount=0;
  expired.channel.onbufferedamountlow();
  await settle();
  assert.equal(expired.sent.length,0,'expired frame sent after buffer drain');
});
test('trace consumer stalls cannot build an unbounded message queue or stop sends',async()=>{
  const f=fixture();let seq=0;f.reply(()=>f.frame(++seq));
  await f.tick();f.view();
  for(let i=0;i<100;i++){await f.tick();f.view()}
  assert.equal(f.sent.length,101);assert.equal(f.messages.filter(m=>m.type==='trace').length,1);
  assert.equal(f.messages.filter(m=>m.type==='view').length,1,'blocked page accumulated status views');
  f.self.onmessage({data:{type:'view_ack',owner:1}});f.view();
  assert.equal(f.messages.filter(m=>m.type==='view').length,2);
  const first=f.messages.find(m=>m.type==='trace');f.self.onmessage({data:{type:'trace_ack',owner:1,sequence:first.sequence}});f.view();
  const batch=f.messages.filter(m=>m.type==='trace').at(-1);assert.equal(batch.records.length,64);assert.equal(batch.dropped,36);
});
test('unsupported transfer preserves page courier and runtime failure closes transferred channel',async()=>{
  const originalLocation=global.location;global.location={origin:'http://localhost:18872'};
  try {
    const raw={label:'control',readyState:'open'};
    class Failed {postMessage(){throw Object.assign(Error('no transfer'),{name:'DataCloneError'})}terminate(){this.terminated=true}}
    const fallback=adapter.create({transport,WorkerClass:Failed});assert.equal(fallback.adopt(raw),raw);fallback.retire();
    let instance;class Worker {constructor(){instance=this}postMessage(message){this.last=message}terminate(){this.terminated=true}}
    const modes=[],actor=adapter.create({transport,WorkerClass:Worker,onMode:value=>modes.push(value)});
    const facade=actor.adopt(raw);assert.notEqual(facade,raw);assert.equal(instance.last.channel,raw);
    assert.throws(()=>facade.send('anything'),/owned/);
    let closed=0;facade.onclose=()=>closed++;instance.onerror();
    assert.equal(facade.readyState,'closed');assert.equal(closed,1);assert.equal(instance.terminated,true);
    assert.equal(modes.at(-1).context,'worker');actor.retire();
  } finally {global.location=originalLocation}
});
test('old worker callbacks and diagnostic failures cannot affect a replacement actor',async()=>{
  const originalLocation=global.location;global.location={origin:'http://localhost:18872'};
  try {
    const workers=[];class Worker {constructor(){workers.push(this)}postMessage(message){(this.messages??=[]).push(message)}terminate(){this.terminated=true}}
    const actor=adapter.create({transport,WorkerClass:Worker,onMode:()=>{throw Error('diagnostic')}});
    actor.createCourier({scope:()=>({session:'s',generation:1,ui:'u',uiEpoch:1,attempt:'a'}),now:()=>0,onTrace:()=>{throw Error('diagnostic')}});
    const raw={label:'control',readyState:'open'};const a=actor.adopt(raw);const b=actor.adopt(raw);
    workers[0].onmessage({data:{owner:1,type:'channel',state:'open'}});assert.equal(a.readyState,'connecting');assert.equal(b.readyState,'connecting');
    workers[1].onmessage({data:{owner:2,type:'trace',sequence:1,records:[{}]}});
    assert.equal(workers[1].messages.at(-1).type,'trace_ack');assert.equal(workers[0].terminated,true);actor.retire();
  } finally {global.location=originalLocation}
});
(async()=>{for(const {name,run} of tests){await run();console.log('PASS '+name)}})().catch(error=>{console.error(error);process.exitCode=1});
