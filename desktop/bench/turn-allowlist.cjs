'use strict';
const {lookup} = require('node:dns/promises');
const {iceAddressInfo} = require('../../cpp/web/control_logic.js');

// Resolve once per run and retain the snapshot with the bench metadata. The
// operator supplies allocation pools separately: they need not equal DNS IPs.
async function resolveTurnAllowlist(config, resolve = lookup) {
  if (!config?.allowRelay) return config;
  if (!Array.isArray(config.turnServers) || !config.turnServers.length || config.turnServers.length>16)
    throw Error('relay arm requires an explicit TURN allowlist');
  const turnServers = await Promise.all(config.turnServers.map(async server => {
    const match=/^turns?:(?:\[([0-9a-f:]+)\]|([a-z\d.-]+))(?::(\d+))?(?:\?transport=(udp|tcp))?$/i.exec(server.url || '');
    if (!match) throw Error('invalid TURN allowlist URL');
    const host=match[1]||match[2], literal=iceAddressInfo(host);
    const results=literal.family==='unknown' ? await resolve(host,{all:true}) : [{address:host}];
    if (!results.length || results.some(row=>iceAddressInfo(row.address).scope!=='public')) throw Error('TURN DNS must resolve only to public addresses');
    return {url:server.url,relayAddresses:server.relayAddresses,
      serverAddresses:[...new Set(results.map(row=>iceAddressInfo(row.address).address))]};
  }));
  return {...config,turnServers};
}
module.exports={resolveTurnAllowlist};
