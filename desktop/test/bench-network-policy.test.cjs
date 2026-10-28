const {test} = require('node:test');
const assert = require('node:assert/strict');
const {createNetworkPolicy:factory} = require('../bench/network-policy.cjs');
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
  const fallback=createNetworkPolicy({...v6,allowRelay:true});
  fallback.requirePair({...pair,remote_type:'relay',remote_address:'2606:4700::3'});
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
