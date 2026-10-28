'use strict';

// Serialized into the opt-in bench page; keep this factory browser-safe.
// Public candidates alone do not prove independent uplinks (NAT hairpin/VPN).
function createNetworkPolicy(config) {
  const ipv4 = value => {
    if (typeof value !== 'string' || !/^(\d{1,3}\.){3}\d{1,3}$/.test(value)) return null;
    const parts = value.split('.').map(Number);
    return parts.every((v, i) => v <= 255 && String(v) === value.split('.')[i]) ? parts : null;
  };
  const publicV4 = value => {
    const p = ipv4(value);
    if (!p) return false;
    const [a,b,c] = p;
    return !(a === 0 || a === 10 || a === 127 || a >= 224 ||
      (a === 100 && b >= 64 && b <= 127) || (a === 169 && b === 254) ||
      (a === 172 && b >= 16 && b <= 31) ||
      (a === 192 && (b === 168 || (b === 0 && (c === 0 || c === 2)) || (b === 88 && c === 99))) ||
      (a === 198 && (b === 18 || b === 19 || (b === 51 && c === 100))) ||
      (a === 203 && b === 0 && c === 113));
  };
  const local = ipv4(config?.localIp);
  const localV4 = local && (publicV4(config.localIp) || local[0] === 10 ||
    (local[0] === 172 && local[1] >= 16 && local[1] <= 31) || (local[0] === 192 && local[1] === 168));
  const directTypes = ['host', 'srflx', 'prflx'];
  if (!config || config.kind !== 'wan-ipv4-udp' || !localV4 ||
      !publicV4(config.localPublicIp) || !publicV4(config.remotePublicIp) ||
      config.localPublicIp === config.remotePublicIp)
    throw Error('WAN bench requires a local IPv4 and two distinct public IPv4 egress addresses');
  // Deliberately narrow bench input: explicitly resolve a real STUN server to
  // IPv4 beforehand. No placeholder, DNS/address-family ambiguity or port 9.
  const stun = /^stun:([\d.]+):(3478|19302)$/.exec(config.stunUrl || '');
  if (!stun || !publicV4(stun[1])) throw Error('WAN bench requires a real public IPv4 STUN endpoint on port 3478 or 19302');
  function candidateAllowed(candidate) {
    const value = typeof candidate === 'string' ? candidate : candidate?.candidate;
    if (value === '') return true; // end-of-candidates
    if (typeof value !== 'string') return false;
    const fields = value.replace(/^a=/, '').trim().split(/\s+/);
    return fields[0].startsWith('candidate:') && fields[2]?.toLowerCase() === 'udp' &&
      fields[4] === config.remotePublicIp && fields[6] === 'typ' && directTypes.includes(fields[7]);
  }
  function requirePair(pair) {
    if (!pair || pair.local_transport !== 'udp' || pair.remote_transport !== 'udp' ||
        ![config.localIp, config.localPublicIp].includes(pair.local_address) ||
        pair.remote_address !== config.remotePublicIp ||
        !directTypes.includes(pair.local_type) || !directTypes.includes(pair.remote_type))
      throw Error('selected pair differs from required public IPv4/UDP endpoints: ' + JSON.stringify(pair));
    return pair;
  }
  function requireIceServers(servers) {
    const urls = (servers || []).flatMap(server => server.urls || []);
    if (urls.length !== 1 || urls[0] !== config.stunUrl)
      throw Error('bench signaling must advertise the same single STUN endpoint to both peers, without TURN');
  }
  return {candidateAllowed, requirePair, requireIceServers, iceServers:[{urls:config.stunUrl}],
    filterSdp: sdp => sdp.split(/\r?\n/).filter(line => !line.startsWith('a=candidate:') || candidateAllowed(line)).join('\r\n')};
}
module.exports = {createNetworkPolicy};
