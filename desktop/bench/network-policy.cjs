'use strict';

// Serialized into the bench page; inspectAddress is the product's shared parser.
// Candidate evidence cannot prove independent physical uplinks or rule out VPNs.
function createNetworkPolicy(input, inspectAddress) {
  const legacy = input?.kind === 'wan-ipv4-udp';
  const config = legacy ? {kind:'wan-dualstack', family:'ipv4', allowRelay:false,
    localAddresses:[input.localIp], localPublicAddresses:[input.localPublicIp],
    remotePublicAddresses:[input.remotePublicIp], stunUrls:[input.stunUrl]} : input;
  if (!config || config.kind !== 'wan-dualstack' || !['ipv4','ipv6','dual'].includes(config.family) ||
      typeof config.allowRelay !== 'boolean') throw Error('WAN bench requires an explicit address family and relay policy');
  const allowedFamily = info => ['ipv4','ipv6'].includes(info.family) && (config.family==='dual'||info.family===config.family);
  const addresses = (values, publicOnly) => {
    if (!Array.isArray(values) || !values.length || values.length>16) throw Error('WAN bench requires bounded IPv4/IPv6 endpoint lists');
    return values.map(value=>{
      const info=inspectAddress(value);
      if (!allowedFamily(info) || !(publicOnly ? info.scope==='public' : ['public','private','shared_cgnat'].includes(info.scope)))
        throw Error('WAN bench requires a local IPv4/IPv6 and distinct public IPv4/IPv6 egress addresses');
      return info.address;
    });
  };
  const local=addresses(config.localAddresses,false), localPublic=addresses(config.localPublicAddresses,true), remote=addresses(config.remotePublicAddresses,true);
  if (localPublic.some(ip=>remote.includes(ip))) throw Error('WAN bench requires distinct public IPv4/IPv6 egress addresses');
  if (config.family==='dual' && !['ipv4','ipv6'].every(f=>local.some(ip=>inspectAddress(ip).family===f) && remote.some(ip=>inspectAddress(ip).family===f)))
    throw Error('dual-stack bench requires both address families at both endpoints');
  const legacyUrl = legacy ? input.stunUrl : input.legacyStunUrl;
  const normalized={...config,legacyStunUrl:legacyUrl,localAddresses:local,localPublicAddresses:localPublic,remotePublicAddresses:remote};
  const directTypes=['host','srflx','prflx'];
  function validServer(url) {
    // Port 9 was the historical Chromium socket failure. No placeholder is
    // admitted, while hostname-based and bracketed IPv6 services are supported.
    const match=/^(stun|stuns|turn|turns):(?:\[([^\]]+)\]|([^:/?]+))(?::(\d+))?(?:\?transport=(udp|tcp))?$/.exec(url || '');
    if (!match || match[3]?.includes('@')) return false;
    const port=Number(match[4] || (match[1].endsWith('s')?5349:3478));
    if (!Number.isInteger(port)||port<1||port>65535||port===9) return false;
    const host=match[2]||match[3], info=inspectAddress(host);
    return info.family==='unknown' ? host!=='localhost' && !host.endsWith('.localhost') &&
      !/^[\d.]+$/.test(host) && /^[a-z\d.-]+$/i.test(host) : info.scope==='public';
  }
  if (legacyUrl && (!/^stun:[\d.]+:(3478|19302)$/.test(legacyUrl) || !validServer(legacyUrl)))
    throw Error('WAN bench requires a real public IPv4 STUN endpoint on port 3478 or 19302');
  // TURN listening addresses and allocation pools can differ. Require both;
  // never infer the pool from DNS, nor infer a relay leg from candidate UDP.
  const publicIps = values => {
    if (!Array.isArray(values)||!values.length||values.length>16) throw Error('TURN requires bounded DNS and relay address lists');
    return values.map(value=>{const info=inspectAddress(value);if(info.scope!=='public')throw Error('TURN addresses must be public');return info.address;});
  };
  const configuredTurn=config.turnServers||[];
  if (!Array.isArray(configuredTurn)||configuredTurn.length>16) throw Error('TURN allowlist must contain at most 16 servers');
  const turnServers=configuredTurn.map(server=>{
    if (!validServer(server.url)||!/^turns?:/.test(server.url)) throw Error('invalid TURN allowlist URL');
    return {url:server.url,serverAddresses:publicIps(server.serverAddresses),relayAddresses:publicIps(server.relayAddresses)};
  });
  if (config.allowRelay && !turnServers.length) throw Error('relay arm requires an explicit TURN allowlist with DNS evidence');
  normalized.turnServers=turnServers;
  let signaledTurnUrls=[];
  function requireIceServers(servers, observe = true) {
    const urls=(servers||[]).flatMap(server=>server.urls||[]);
    if (urls.some(url=>!validServer(url))) throw Error('invalid or placeholder STUN/TURN endpoint');
    if (legacyUrl && (urls.length!==1 || urls[0]!==legacyUrl))
      throw Error('legacy bench signaling must advertise the same single STUN endpoint without TURN');
    if (config.allowRelay && !urls.some(url=>/^turns?:/.test(url))) throw Error('fallback bench requires TURN from authenticated signaling');
    if (config.allowRelay && urls.filter(url=>/^turns?:/.test(url)).some(url=>!turnServers.some(server=>server.url===url)))
      throw Error('signaling TURN server is outside the explicit allowlist');
    if (observe) signaledTurnUrls=urls.filter(url=>/^turns?:/.test(url));
  }
  function candidateAllowed(candidate) {
    const value=typeof candidate==='string'?candidate:candidate?.candidate;
    if (value==='') return true;
    if (typeof value!=='string') return false;
    const fields=value.replace(/^a=/,'').trim().split(/\s+/), info=inspectAddress(fields[4]);
    return fields[0].startsWith('candidate:') && fields[2]?.toLowerCase()==='udp' && fields[6]==='typ' &&
      allowedFamily(info) && info.scope==='public' &&
      (fields[7]==='relay' ? config.allowRelay && turnServers.some(server=>signaledTurnUrls.includes(server.url)&&server.relayAddresses.includes(info.address))
        : directTypes.includes(fields[7]) && remote.includes(info.address));
  }
  function relayAllowed(pair, side, info) {
    const server=turnServers.find(item=>item.url===pair[side+'_url']);
    return config.allowRelay && info.scope==='public' && pair[side+'_relay_protocol']==='udp' &&
      server && /^turn:/.test(server.url) && !server.url.endsWith('transport=tcp') &&
      signaledTurnUrls.includes(server.url) && server.relayAddresses.includes(info.address);
  }
  function requirePair(pair) {
    const l=inspectAddress(pair?.local_address), r=inspectAddress(pair?.remote_address);
    if (!pair || pair.local_transport!=='udp' || pair.remote_transport!=='udp' || !allowedFamily(l) || !allowedFamily(r) || l.family!==r.family ||
        ![...directTypes,'relay'].includes(pair.local_type) || ![...directTypes,'relay'].includes(pair.remote_type) ||
        (pair.local_type==='relay' ? !relayAllowed(pair,'local',l) : !local.concat(localPublic).includes(l.address)) ||
        (pair.remote_type==='relay' ? !relayAllowed(pair,'remote',r) : !remote.includes(r.address)))
      throw Error('selected pair differs from required public IPv4/IPv6 UDP endpoints or relay policy: '+JSON.stringify(pair));
    return pair;
  }
  return {config:normalized,candidateAllowed,requirePair,requireIceServers,signaledTurnUrls:()=>[...signaledTurnUrls],
    // Direct-only A/B excludes relay on both sides of the receiver. Sender
    // configuration must separately match the chosen diagnostic experiment.
    serversForReceiver:servers=>servers.map(server=>({...server,urls:(Array.isArray(server.urls)?server.urls:[server.urls])
      .filter(url=>config.allowRelay||!/^turns?:/.test(url))})).filter(server=>server.urls.length),
    filterSdp:sdp=>sdp.split(/\r?\n/).filter(line=>!line.startsWith('a=candidate:')||candidateAllowed(line)).join('\r\n')};
}
module.exports={createNetworkPolicy};
