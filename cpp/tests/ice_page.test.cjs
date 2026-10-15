// Executes the shipped page methods from server.cpp. Peer/HTTP fixtures never
// create sockets. Direct/TURN cases model connectivity, not real ICE checks.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const controlLogic = require('../web/control_logic.js');
const sourcePath = path.resolve(__dirname, '../src/server.cpp');
const source = fs.readFileSync(sourcePath, 'utf8');
function section(start, end) {
  const begin = source.indexOf(start), stop = source.indexOf(end, begin);
  assert(begin >= 0 && stop > begin, `production boundary missing: ${start}`);
  return source.slice(begin, stop);
}
const methods = section('async function pollSignaling(', '\nfunction currentMediaPeer(') + '\n' + section('function applyMediaStatus(', '\nasync function pollSignaling(') + '\n' + section('function currentMediaPeer(', '\nfunction offeredVideoCameraIds(') + '\n' +
  section('async function startFromOffer(', '\nsetInterval(()=>cameraView.refresh()') + '\n' +
  section('async function collectMetrics(){', '\nsetInterval(()=>collectMetrics()');
function deferred() {
  let resolve, reject;
  const promise = new Promise((a, b) => { resolve = a; reject = b; });
  return {promise, resolve, reject};
}
function fixture() {
  const posts = [], peers = [], logs = [], gates = {}, resets = [];
  class Peer {
    constructor(config) { this.config = config; this.added = []; this.connectionState = 'connected'; peers.push(this); }
    close() { this.closed = true; }
    async setRemoteDescription(value) { if (gates.remote) { gates.remote.entered=true; await gates.remote.promise; } this.remoteDescription = value; }
    async createAnswer() { if (gates.answer) { gates.answer.entered=true; await gates.answer.promise; } return {type:'answer', sdp:'v=0\r\na=ice-ufrag:driver\r\n'}; }
    async setLocalDescription(value) { if (gates.local) { gates.local.entered=true; await gates.local.promise; } this.localDescription = value; }
    async addIceCandidate(value) { if (gates.candidate) { gates.candidate.entered=true; await gates.candidate.promise; } this.added.push(value); }
    async getStats() { if (gates.stats) { gates.stats.entered=true; await gates.stats.promise; } return this.stats || new Map(); }
  }
  const context = vm.createContext({
    MineTeleopControlTransport:require('../web/control_transport.js'),setInterval(){},
    peer:null,controlChannel:null,pendingIce:[],mediaPeerEpoch:0,mediaOfferScope:null,mediaHasOffer:false,
    retiredMediaScopes:new Set(),mediaScopeIssue:'',nativeControlSessionId:'session-fixture',nativeControlSessionGeneration:7,
    lastControlStatusSeq:0,cameraByMid:new Map(),assignedCameraIds:new Set(),previousStats:new Map(),
    remoteCameraIds:[],h265FailureSamples:0,h265FallbackSent:false,
    iceServers:[{urls:['stun:fixture.invalid','turn:fixture.invalid']}],consoleConfig:{ice_transport_policy:'all'},
    RTCPeerConnection:Peer,cameraView:{configure(){},setConnectionState(){}},webrtcLabel:{},
    updateVcuHandshakeState(){},resetControlProfileSession(){resets.push('profile')},resetControlAuthorityInput(){resets.push('input')},
    offeredVideoCameraIds(){return new Map()},clientLog(event,details){logs.push({event,details})},renderMonitoring(){},
    post:async(route,body)=>{posts.push({route,body}); if (gates.post) await gates.post.promise; return {}},
    console,safeIceEndpoint(){return 'fixture'},controlLogic,performance,AbortController,setTimeout:callback=>callback(),
    polling:false,signalingGeneration:11,signalingPollAbort:null,controlAuthorityLost:false,
    pageReleaseScope(){return {}},safeLogoutReleaseBody(){return {}},closeRealtimeSession(){context.closed=(context.closed||0)+1;context.polling=false},
    mediaStatus:{lanes:[]},latencyBreakdown:{},estimatedLatency:{},timeSync:{},latestMetrics:null,
    currentTimeSync(){return {synchronized:true,uncertainty_ms:1}},controlOutcomeSnapshot(){return {}},
    controlTraceScope:{session_id:'session-fixture',vehicle_id:'vehicle-fixture'},lastMediaMetricsLogAt:null,
    selectedCodec:'h264',renderMetrics(){},renderLatency(){},flushControlTrace(){},attach(){},
    latestRuntimeStatus:{time_sync:{synchronized:true,uncertainty_ms:1}},hasTurnServer(){return true},
    controlOutcomeSession:{metrics:controlLogic.createControlOutcomeMetrics()},statusPanel:{},formatMetric(){return 'fixture'},
  });
  vm.runInContext(methods, context, {filename:'actual-shipped-ice-methods'});
  return {context, peers, posts, logs, gates, resets};
}
const offer = (id, ufrag=id) => ({media_negotiation_id:id,sdp:`v=0\r\na=ice-ufrag:${ufrag}\r\n`,media_tracks:[]});
const candidate = (id, ufrag=id, type='host') => ({media_negotiation_id:id,usernameFragment:ufrag,
  candidate:`candidate:fixture 1 udp 100 127.0.0.1 9000 typ ${type}`,sdpMLineIndex:0});
const icePosts = f => f.posts.filter(p=>p.route==='/api/webrtc/ice-candidate');
const tests = [];
function test(id, fn) { tests.push({id,fn}); }
test('ICE-PAGE-001', async()=>{
  const f=fixture(); await f.context.addIce(candidate('attempt-a'));
  await f.context.startFromOffer(offer('attempt-a'));
  assert.equal(f.peers[0].added.length,1,'matching pre-offer ICE was lost');
  assert.equal(f.context.pendingIce.length,0);
});
test('ICE-PAGE-002', async()=>{
  const f=fixture(); await f.context.startFromOffer(offer('a')); const old=f.peers[0];
  await f.context.startFromOffer(offer('b')); const initialLogs=f.logs.length, initialResets=f.resets.length;
  old.onicecandidate({candidate:{toJSON:()=>candidate('a')}});
  old.onicecandidateerror({url:'turn:fixture.invalid',errorCode:701});
  old.connectionState='failed'; old.onconnectionstatechange(); old.ontrack({track:{kind:'video'}});
  const channel={label:'invalid',close(){this.closed=true}};old.ondatachannel({channel});
  await Promise.resolve(); assert.equal(icePosts(f).length,0);assert.equal(f.logs.length,initialLogs);
  assert.equal(f.resets.length,initialResets);assert.equal(channel.closed,true);assert.equal(old.closed,true);
  f.peers[1].onicecandidate({candidate:{toJSON:()=>candidate('b')}});await Promise.resolve();
  assert.equal(icePosts(f).length,1); assert.equal(icePosts(f)[0].body.media_negotiation_id,'b');
  assert.equal(icePosts(f)[0].body.source_control_session_generation,7);
});
test('ICE-PAGE-003', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));
  await f.context.addIce(candidate('future'));await f.context.addIce(candidate('a'));
  await f.context.startFromOffer(offer('b'));await f.context.addIce(candidate('a'));
  assert.equal(f.peers[1].added.length,0,'retired candidate entered replacement');
  assert.equal(f.context.pendingIce.length,1,'future scoped candidate was lost');
  await f.context.startFromOffer(offer('future'));assert.equal(f.peers[2].added.length,1);
  await f.context.startFromOffer(offer('a'));assert.equal(f.peers.length,3,'retired offer revived');
  await f.context.startFromOffer(offer('future'));assert.equal(f.peers.length,3,'duplicate offer replaced current peer');
});
test('ICE-PAGE-004', async()=>{
  const f=fixture();const legacy={candidate:'candidate:fixture 1 udp 1 127.0.0.1 9000 typ host',sdpMLineIndex:0};
  await f.context.addIce(legacy);await f.context.startFromOffer({sdp:'v=0\r\na=ice-ufrag:first\r\n'});
  assert.equal(f.peers[0].added.length,1,'first legacy bootstrap was broken');
  await f.context.startFromOffer({sdp:'v=0\r\na=ice-ufrag:second\r\n'});await f.context.addIce(legacy);
  await f.context.addIce({...legacy,usernameFragment:'first'});assert.equal(f.peers[1].added.length,0);
  await f.context.addIce({...legacy,usernameFragment:'second'});assert.equal(f.peers[1].added.length,1);
});
test('ICE-PAGE-005', async()=>{
  for(const stage of ['remote','answer','local']) {
    const f=fixture();f.gates[stage]=deferred();const gate=f.gates[stage];
    const old=f.context.startFromOffer(offer('a'));
    for(let n=0;n<20&&!gate.entered;n++)await Promise.resolve();assert.equal(gate.entered,true,stage+' gate not reached');
    delete f.gates[stage];await f.context.startFromOffer(offer('b'));gate.reject(Error('stale fixture failure'));
    await old;assert.equal(f.posts.filter(p=>p.route==='/api/webrtc/answer').length,1,stage);
  }
});
test('ICE-PAGE-006', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));f.gates.post=deferred();const gate=f.gates.post;
  f.peers[0].onicecandidate({candidate:{toJSON:()=>candidate('a')}});
  delete f.gates.post;await f.context.startFromOffer(offer('b'));gate.reject(Error('stale fixture post'));
  await Promise.resolve();await Promise.resolve();assert(!f.logs.some(v=>v.event==='webrtc_ice_signal_error'));
  assert.equal(icePosts(f)[0].body.media_negotiation_id,'a','inflight candidate relabelled as replacement');
});
test('ICE-PAGE-007', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));f.gates.stats=deferred();const gate=f.gates.stats;
  const old=f.context.collectMetrics();delete f.gates.stats;await f.context.startFromOffer(offer('b'));
  gate.reject(Error('stale stats fixture'));await old;
  assert.equal(f.posts.filter(p=>p.route==='/api/webrtc/metrics').length,0);
  assert.equal(f.context.latestMetrics,null);assert.equal(f.context.previousStats.size,0);
});
test('ICE-PAGE-008', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));const old=f.peers[0];
  f.context.nativeControlSessionGeneration=8;old.onicecandidate({candidate:{toJSON:()=>candidate('a')}});
  old.onicecandidateerror({url:'turn:fixture.invalid',errorCode:701});assert.equal(icePosts(f).length,0);assert.equal(f.logs.length,0);
  for(let n=0;n<128;n++)await f.context.addIce(candidate(`future-${n}`));
  await assert.rejects(f.context.addIce(candidate('overflow')),/buffer exceeds limit/);assert.equal(f.context.pendingIce.length,128);
});
test('ICE-PAGE-009', async()=>{
  const f=fixture();f.gates.remote=deferred();const gate=f.gates.remote;
  const active=f.context.startFromOffer({sdp:'v=0\r\na=ice-ufrag:legacy\r\n'});
  await f.context.addIce({candidate:'candidate:fixture 1 udp 1 127.0.0.1 9000 typ host',sdpMLineIndex:0});
  gate.resolve();await active;assert.equal(f.peers[0].added.length,1,'legacy ICE arriving during description was dropped');
});
test('ICE-PAGE-010', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));f.gates.post=deferred();const gate=f.gates.post;
  const old=f.context.collectMetrics();for(let n=0;n<20&&!f.posts.some(p=>p.route==='/api/webrtc/metrics');n++)await Promise.resolve();
  assert(f.posts.some(p=>p.route==='/api/webrtc/metrics'));
  delete f.gates.post;await f.context.startFromOffer(offer('b'));gate.resolve();await old;
  assert.equal(f.context.latestMetrics,null,'old metrics response changed replacement UI');
});
test('ICE-MIXED-PAGE-001', async()=>{
  const f=fixture();const legacy={candidate:'candidate:fixture 1 udp 1 127.0.0.1 9000 typ host',sdpMLineIndex:0};
  await f.context.startFromOffer({sdp:'v=0\r\na=ice-ufrag:old-first\r\n'});await f.context.addIce(legacy);
  assert.equal(f.peers[0].added.length,1,'legacy bootstrap failed');
  await f.context.startFromOffer({sdp:'v=0\r\na=ice-ufrag:old-retry\r\n'});
  await f.context.addIce(legacy);await f.context.addIce(legacy);
  assert.equal(f.peers[1].added.length,0);assert.equal(f.context.mediaScopeIssue,'media_ice_scope_missing');
  assert.match(f.context.statusPanel.textContent,/统一更新控制端和车端包/);
  assert.equal(f.logs.filter(v=>v.event==='driver_media_scope_rejected').length,1,'scope warning repeated per candidate');
  await f.context.addIce({...legacy,usernameFragment:'old-retry'});assert.equal(f.peers[1].added.length,1,'known legacy ufrag filtering was weakened');
});
test('ICE-MIXED-PAGE-002', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('b'));
  f.context.applyMediaStatus({media_negotiation:{id:'a',issue_code:'media_negotiation_scope_missing'}});
  assert.equal(f.context.mediaStatus.media_negotiation,undefined,'stale offer status updated current media state');
  f.context.applyMediaStatus({media_negotiation:{id:'b',issue_code:'media_negotiation_scope_missing'}});
  assert.match(f.context.statusPanel.textContent,/重新打开控制页面并建立新会话/);
  assert.equal(controlLogic.mediaScopeDiagnostic('unknown-code'),null);
});
test('ICE-POLL-BATCH-001', async()=>{
  const f=fixture();f.context.polling=true;const batches=[
    {type:'ice_candidate',payload:candidate('a')},{type:'webrtc_offer',payload:offer('a')},
    {type:'ice_candidate',payload:candidate('b')},{type:'webrtc_offer',payload:offer('b')},
    {type:'ice_candidate',payload:candidate('a')},{type:'ice_candidate',payload:candidate('b')}];
  f.context.post=async(route,body)=>{f.posts.push({route,body});if(route==='/api/poll-signaling'){f.context.polling=false;return {messages:batches}}
    if(route==='/api/webrtc/answer'&&body.media_negotiation_id!=='b')throw Error('current native offer is b');return {}};
  await f.context.pollSignaling(11);assert.equal(f.peers.length,1,'obsolete offer created a peer');
  assert.equal(f.peers[0].added.length,2,'latest offer scoped ICE lost');assert.equal(f.context.closed,undefined,'correct latest-offer fence closed session');
  assert.equal(f.posts.filter(p=>p.route==='/api/end-session').length,0);assert.equal(f.context.mediaOfferScope.id,'b');
});
test('ICE-ROUTE-SIM-001', async()=>{
  // Connectivity outcome is explicitly supplied by the fixture. This tests
  // ordering + diagnostics with all policy; it does not prove UDP/NAT paths.
  for(const directReachable of [true,false]) {
    const f=fixture();await f.context.addIce(candidate('a','a','host'));await f.context.addIce(candidate('a','a','relay'));
    await f.context.startFromOffer(offer('a'));assert.equal(f.peers[0].config.iceTransportPolicy,'all');
    assert.deepEqual(f.peers[0].added.map(v=>v.candidate.split(' typ ')[1]),['host','relay']);
    const report=new Map([['transport',{id:'transport',type:'transport',selectedCandidatePairId:'selected'}],
      ['selected',{id:'selected',type:'candidate-pair',state:'succeeded',nominated:true,localCandidateId:'local',remoteCandidateId:'remote',currentRoundTripTime:0.03}],
      ['local',{id:'local',type:'local-candidate',candidateType:directReachable?'host':'relay',protocol:'udp',relayProtocol:directReachable?undefined:'tcp'}],
      ['remote',{id:'remote',type:'remote-candidate',candidateType:directReachable?'host':'relay',protocol:'udp'}]]);
    f.peers[0].stats=report;await f.context.collectMetrics();
    const reported=f.posts.find(p=>p.route==='/api/webrtc/metrics').body;
    assert.equal(reported.connection_method,directReachable?'direct':'TURN');assert.equal(reported.control_rtt_ms,30);
    assert.equal(reported.source_session_id,'session-fixture');assert.equal(reported.media_negotiation_id,'a');
    const selected=controlLogic.selectedIcePair(report);assert.equal(selected.pair.id,'selected');
    const diagnostic=controlLogic.iceConnectivity(report,selected.pair);assert.equal(diagnostic.pairs.find(p=>p.selected).local_type,directReachable?'host':'relay');
  }
});
test('ICE-PATH-METRICS-001', async()=>{
  const f=fixture();await f.context.startFromOffer(offer('a'));
  const report=new Map([['t',{type:'transport',selectedCandidatePairId:'p'}],
    ['p',{id:'p',type:'candidate-pair',state:'succeeded',localCandidateId:'l',remoteCandidateId:'r',currentRoundTripTime:0.05}],
    ['l',{type:'local-candidate',candidateType:'relay',protocol:'udp',relayProtocol:'tcp'}],
    ['r',{type:'remote-candidate',candidateType:'relay',protocol:'udp'}]]);
  f.peers[0].stats=report;await f.context.collectMetrics();
  const value=f.posts.find(p=>p.route==='/api/webrtc/metrics').body;
  assert.equal(value.network_path,'turn_tcp');assert.equal(value.media_transport_rtt_ms,50);
  assert.equal(value.control_rtt_source,'media_ice_pair_legacy_field');
  assert.equal(value.ice_connectivity.path_checks.turn_tcp.selected_pairs,1);
  report.delete('r');await f.context.collectMetrics();
  const unknown=f.posts.filter(p=>p.route==='/api/webrtc/metrics').at(-1).body;
  assert.equal(unknown.network_path,'unknown');assert.equal(unknown.connection_method,'TURN');
  assert.equal(unknown.turn_in_use,true,'known local relay must remain compatible with native ingress');
});
(async()=>{
  const results=[];
  for(const {id,fn} of tests) {
    try { await fn();results.push({id,status:'passed'}); }
    catch(error) { results.push({id,status:'failed',error:error.stack}); }
  }
  console.log(JSON.stringify({scope:'actual shipped page methods; Peer/HTTP mocked; no sockets or UDP reachability proof',
    source_sha256:crypto.createHash('sha256').update(source).digest('hex'),results},null,2));
  process.exitCode=results.every(v=>v.status==='passed')?0:1;
})().catch(error=>{console.error(error);process.exitCode=2});
