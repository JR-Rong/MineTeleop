const {test} = require('node:test');
const assert = require('node:assert/strict');
const {createNetworkPolicy:factory,validatePhysicalInterface} = require('../bench/network-policy.cjs');
const {iceAddressInfo} = require('../../cpp/web/control_logic.js');
const createNetworkPolicy = config => factory(config, iceAddressInfo);
// Parser fixtures only: no packets are sent to these addresses.
const config = {kind:'wan-ipv4-udp',localIp:'192.168.1.2',localPublicIp:'8.8.8.8',remotePublicIp:'1.1.1.1',stunUrl:'stun:9.9.9.9:3478'};
const policy = createNetworkPolicy(config);
test('WAN filter excludes LAN, IPv6, TCP and TURN while preserving session fences', () => {
  const good = 'candidate:1 1 UDP 100 1.1.1.1 50000 typ srflx raddr 10.0.0.2 rport 50000';
  const candidates = [good, good.replace('UDP','TCP'), good.replace('1.1.1.1','10.0.0.2'),
    good.replace('1.1.1.1','fe80::1'), good.replace('typ srflx','typ relay')];
  const sdp = ['v=0','a=ice-ufrag:fence',...candidates.map(c=>'a='+c),''].join('\r\n');
  assert.equal(policy.filterSdp(sdp), ['v=0','a=ice-ufrag:fence','a='+good,''].join('\r\n'));
  assert.deepEqual(candidates.map(candidate=>policy.candidateAllowed({candidate})), [true,false,false,false,false]);
  assert.equal(policy.candidateAllowed({candidate:''}), true);
  assert.equal(policy.candidateAllowed({}), false);
});
test('actual pair rejects fallback, unknown addresses, wrong NIC and relay', () => {
  const pair = {local_address:config.localIp,remote_address:config.remotePublicIp,local_transport:'udp',remote_transport:'udp',local_type:'host',remote_type:'srflx'};
  assert.equal(policy.requirePair(pair), pair);
  policy.requirePair({...pair,local_address:config.localPublicIp,local_type:'srflx'});
  for (const change of [{local_transport:'tcp'},{remote_address:'192.168.2.2'},{local_address:''},{local_address:'198.18.0.1'},{remote_type:'relay'}])
    assert.throws(()=>policy.requirePair({...pair,...change}), /public IPv4\/IPv6 UDP/);
});
test('private, CGNAT, special-use, noncanonical and same egress addresses fail closed', () => {
  for (const remotePublicIp of ['10.0.0.1','172.16.1.1','192.168.2.2','127.0.0.1','100.64.0.1','169.254.1.1','198.18.0.1','192.0.2.1','198.51.100.1','203.0.113.1','224.0.0.1','240.0.0.1','1.1.01.1','1.1.1.999','::1',config.localPublicIp])
    assert.throws(()=>createNetworkPolicy({...config,remotePublicIp}), /distinct public IPv4/);
  const cellular=createNetworkPolicy({...config,localIp:'100.64.0.2'});
  cellular.requirePair({local_address:'100.64.0.2',remote_address:config.remotePublicIp,local_transport:'udp',remote_transport:'udp',local_type:'host',remote_type:'srflx'});
  for (const localIp of ['127.0.0.1','198.18.0.1','0.0.0.0','::1'])
    assert.throws(()=>createNetworkPolicy({...config,localIp}), /local IPv4/);
});
test('restricted discard port and mismatched sender STUN configuration cannot enter measurement', () => {
  for (const stunUrl of ['stun:127.0.0.1:9','stun:9.9.9.9:9','stun:127.0.0.1:3478','turn:9.9.9.9:3478','stun:example.test:3478'])
    assert.throws(()=>createNetworkPolicy({...config,stunUrl}), /STUN endpoint/);
  policy.requireIceServers([{urls:[config.stunUrl]}]);
  policy.requireIceServers([{urls:config.stunUrl}]);
  for (const servers of [[],[{urls:'stun:127.0.0.1:9'}],[...[{urls:config.stunUrl}],{urls:'turn:9.9.9.9:3478'}]])
    assert.throws(()=>policy.requireIceServers(servers), /STUN/);
});
test('IPv6 and dual-stack WAN support canonical addresses, global host candidates and explicit relay fallback', () => {
  const v6={kind:'wan-dualstack',family:'ipv6',allowRelay:false,localAddresses:['2001:4860::1'],localPublicAddresses:['2001:4860::1'],remotePublicAddresses:['2606:4700::2']};
  const p=createNetworkPolicy(v6), candidate='candidate:v6 1 UDP 100 2606:4700:0:0:0:0:0:2 50000 typ host';
  assert.equal(p.candidateAllowed(candidate),true);
  assert.equal(p.candidateAllowed(candidate.replace('2606:4700:0:0:0:0:0:2','fe80::2')),false);
  const pair={local_address:'2001:4860::1',remote_address:'2606:4700::2',local_transport:'udp',remote_transport:'udp',local_type:'host',remote_type:'host'};
  p.requirePair(pair);
  p.requireIceServers([]); // Global IPv6 host can connect without srflx.
  assert.throws(()=>p.requirePair({...pair,remote_type:'relay'}),/relay policy/);
  const turn={url:'turn:[2606:4700::3]:3478?transport=udp',serverAddresses:['2606:4700::3'],relayAddresses:['2606:4700::3']};
  const fallback=createNetworkPolicy({...v6,allowRelay:true,turnServers:[turn]});
  assert.throws(()=>fallback.requirePair({...pair,remote_type:'relay',remote_address:'2606:4700::3'}),/relay policy/);
  fallback.requireIceServers([{urls:'turn:[2606:4700::3]:3478?transport=udp',username:'u',credential:'p'}]);
  assert.throws(()=>fallback.requireIceServers([]),/requires TURN/);
  const dual=createNetworkPolicy({...v6,family:'dual',localAddresses:[...v6.localAddresses,'192.168.1.2'],localPublicAddresses:[...v6.localPublicAddresses,'8.8.8.8'],remotePublicAddresses:[...v6.remotePublicAddresses,'1.1.1.1']});
  dual.requirePair(pair);
  dual.requirePair({...pair,local_address:'192.168.1.2',remote_address:'1.1.1.1'});
});
test('serialized factory retains legacy STUN guard and does not disclose credentials', () => {
  const normalized=createNetworkPolicy(config).config;
  const factoryCopy=Function('return ('+factory.toString()+')')();
  const addressCopy=Function('return ('+iceAddressInfo.toString()+')')();
  const policy=factoryCopy(normalized,addressCopy);
  assert.throws(()=>policy.requireIceServers([{urls:'stun:8.8.8.8:3478'}]),/same single STUN/);
  assert(!JSON.stringify(policy.config).includes('credential'));
});

test('UDP relay requires every relay leg and an attested TURN pool; non-relay endpoint stays pinned', () => {
  const url='turn:turn.example.test:3478?transport=udp';
  const config={kind:'wan-dualstack',family:'ipv4',allowRelay:true,localAddresses:['192.168.1.2'],localPublicAddresses:['8.8.8.8'],remotePublicAddresses:['1.1.1.1'],
    turnServers:[{url,serverAddresses:['9.9.9.9'],relayAddresses:['9.9.9.10']}]};
  const p=createNetworkPolicy(config);
  const pair={local_address:'9.9.9.10',remote_address:'1.1.1.1',local_transport:'udp',remote_transport:'udp',local_type:'relay',remote_type:'srflx',local_relay_protocol:'udp',local_url:url};
  assert.throws(()=>p.requirePair(pair),/relay policy/);
  p.requireIceServers([{urls:url}]); p.requirePair(pair);
  for(const change of [{local_relay_protocol:'tcp'},{local_relay_protocol:'tls'},{local_relay_protocol:undefined},
      {local_url:undefined},{local_url:'turn:other.example.test:3478?transport=udp'}, {local_address:'8.8.4.4'},
      {remote_address:'8.8.4.4'}, {remote_type:'relay',remote_address:'9.9.9.10'}])
    assert.throws(()=>p.requirePair({...pair,...change}),/relay policy/);
  const both={...pair,remote_type:'relay',remote_address:'9.9.9.10',remote_url:url,remote_relay_protocol:'udp'};
  p.requirePair(both);
  for(const protocol of ['tcp','tls',undefined])assert.throws(()=>p.requirePair({...both,remote_relay_protocol:protocol}),/relay policy/);
  assert.equal(p.candidateAllowed('candidate:r 1 UDP 100 8.8.4.4 5000 typ relay'),false);
  assert.throws(()=>p.requireIceServers([{urls:'turn:other.example.test:3478'}]),/allowlist/);
});
test('TURN DNS snapshot is resolved independently from explicit allocation pools', async () => {
  const {resolveTurnAllowlist}=require('../bench/turn-allowlist.cjs');
  const config={allowRelay:true,turnServers:[{url:'turn:turn.example.test:3478',relayAddresses:['9.9.9.10']}]};
  const result=await resolveTurnAllowlist(config,async host=>{assert.equal(host,'turn.example.test');return [{address:'9.9.9.9'}]});
  assert.deepEqual(result.turnServers[0].serverAddresses,['9.9.9.9']);
  assert.deepEqual(result.turnServers[0].relayAddresses,['9.9.9.10']);
  await assert.rejects(resolveTurnAllowlist(config,async()=>[{address:'127.0.0.1'}]),/public/);
  await assert.rejects(resolveTurnAllowlist(config,async()=>[]),/public/);
});
test('IPv6 zones classify without disclosing interface or address in candidate summaries', () => {
  const {iceCandidateSummary}=require('../../cpp/web/control_logic.js');
  assert.deepEqual(iceAddressInfo('fe80::1%en0'),{family:'ipv6',scope:'link_local',address:'fe80::1'});
  const summary=iceCandidateSummary({candidate:'candidate:x 1 UDP 100 fe80::1%en0 5000 typ host'});
  assert.equal(summary.address_class,'link_local');
  assert(!JSON.stringify(summary).includes('en0')); assert(!JSON.stringify(summary).includes('fe80'));
});
test('actual renderer fetch wrapper cannot replace current TURN evidence with a late response', async () => {
  const vm=require('node:vm'), fs=require('node:fs');
  const source=fs.readFileSync(require.resolve('../bench/renderer.js'),'utf8');
  const urls=['turn:first.example.test:3478?transport=udp','turn:second.example.test:3478?transport=udp'];
  const p=createNetworkPolicy({kind:'wan-dualstack',family:'ipv4',allowRelay:true,
    localAddresses:['192.168.1.2'],localPublicAddresses:['8.8.8.8'],remotePublicAddresses:['1.1.1.1'],
    turnServers:urls.map(url=>({url,serverAddresses:['9.9.9.9'],relayAddresses:['9.9.9.10']}))});
  const pending=[];
  const context=vm.createContext({network:p,fetchIceServers:()=>new Promise(resolve=>pending.push(resolve)),
    mediaOfferScope:{id:'old'},nativeControlSessionId:'s',nativeControlSessionGeneration:1});
  vm.runInContext(source.slice(source.indexOf('  if (network) {\n    const originalFetch'),source.indexOf("  if (mode === 'native')")),context);
  const old=context.fetchIceServers();context.mediaOfferScope={id:'new'};
  const fresh=context.fetchIceServers();pending[1]([{urls:urls[1]}]);await fresh;
  pending[0]([{urls:urls[0]}]);await old;
  assert.deepEqual(p.signaledTurnUrls(),[urls[1]]);
});
test('fallback is an explicit validation goal, not a synonym for allowRelay', () => {
  assert.equal(createNetworkPolicy(config).config.validationGoal,'media');
  assert.throws(()=>createNetworkPolicy({...config,validationGoal:'fallback'}),/requires allowRelay/);
  assert.throws(()=>createNetworkPolicy({...config,validationGoal:'anything'}),/validationGoal/);
});
test('physical interface pin is explicit and has a bounded safe name', () => {
  const v6={kind:'wan-dualstack',family:'ipv6',allowRelay:false,localAddresses:['2001:4860::1'],localPublicAddresses:['2001:4860::1'],remotePublicAddresses:['2606:4700::2']};
  assert.equal(createNetworkPolicy({...v6,localInterface:'en0'}).config.localInterface,'en0');
  const pinned=createNetworkPolicy({...v6,localInterface:'en0'});
  const pair={local_address:'2001:4860::1',remote_address:'2606:4700::2',local_transport:'udp',remote_transport:'udp',local_type:'host',remote_type:'host'};
  assert.throws(()=>pinned.requirePair(pair),/selected pair/);
  assert.throws(()=>pinned.requirePair({...pair,interface_bound:false}),/selected pair/);
  pinned.requirePair({...pair,interface_bound:true});
  const interfaces={en0:[{address:'2001:4860::1'}],en1:[{address:'2606:4700::2'}]};
  validatePhysicalInterface(pinned.config,'native',interfaces,iceAddressInfo);
  assert.throws(()=>validatePhysicalInterface(pinned.config,'browser',interfaces,iceAddressInfo),/browser ICE/);
  assert.throws(()=>validatePhysicalInterface({...pinned.config,localInterface:'en1'},'native',interfaces,iceAddressInfo),/belong/);
  for(const localInterface of ['', '../en0', 'en0;command', 'interface-name-too-long', 7])
    assert.throws(()=>createNetworkPolicy({...v6,localInterface}),/invalid local interface/);
});
test('renderer records only accepted current browser attempts, never skipped or superseded offers', async () => {
  const vm=require('node:vm'),fs=require('node:fs');
  const source=fs.readFileSync(require.resolve('../bench/renderer.js'),'utf8');
  let release;
  const context=vm.createContext({mode:'browser',network:policy,mediaOfferScope:null,
    nativeControlSessionId:'s',nativeControlSessionGeneration:2,retiredMediaScopes:new Set(),
    iceServers:[{urls:config.stunUrl}],pendingIce:[],started:false,performance:{now:()=>10},data:{attempts:[]},
    addIce:async()=>{},startFromOffer:async offer=>{
      if(offer.media_negotiation_id==='skip')return;
      context.mediaOfferScope={id:offer.media_negotiation_id};
      if(offer.media_negotiation_id==='late')await new Promise(resolve=>{release=resolve;});
    }});
  vm.runInContext(source.slice(source.indexOf('  const scope ='),source.indexOf('  let stopped')),context);
  vm.runInContext(source.slice(source.indexOf('  if (network) {\n    const originalOffer'),source.indexOf('  function phaseAt')),context);
  const offer=id=>({media_negotiation_id:id,ice_route_mode:'direct',sdp:'v=0\r\n'});
  await context.startFromOffer(offer('skip'));
  assert.equal(context.data.attempts.length,0);
  await context.startFromOffer(offer('good'));
  await context.startFromOffer(offer('good'));
  assert.equal(context.data.attempts.length,1);
  assert.equal(context.data.attempts[0].sourceControlSessionGeneration,2);
  const late=context.startFromOffer(offer('late'));
  context.mediaOfferScope={id:'replacement'};release();await late;
  assert.equal(context.data.attempts.length,1);
  context.started=true;
  await assert.rejects(context.startFromOffer(offer('new')),/stops on renegotiation/);
});
