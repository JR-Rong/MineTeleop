'use strict';
(function expose(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopControlTransport = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createTransport() {
  const protocol = 'native-control-transport-v1';
  function createCourier({scope, post, now, send, onBoundary, onStatus = () => {}}) {
    let busy = false, key = '', lastSeen = 0, revision = 0, result = null, negotiated = false, path = 'wss', epoch = 0;
    const scopeKey = value => JSON.stringify([value.session, value.generation, value.ui, value.uiEpoch, value.attempt]);
    function receive(message) {
      const current = scope();
      if (!current || message?.protocol !== protocol || message.event !== 'vehicle_control_result' ||
          message.session_id !== current.session || !Number.isSafeInteger(message.command_seq) || message.command_seq <= 0 ||
          !Number.isSafeInteger(message.result_seq) || message.result_seq <= 0) return false;
      if (key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; }
      if (!result || message.result_seq > result.result_seq) result = message;
      return true;
    }
    async function tick() {
      if (busy) return;
      const current = scope();
      if (!current || !current.session || !current.attempt || !current.ui || current.uiEpoch <= 0) return;
      const capturedKey = scopeKey(current);
      if (key !== capturedKey) { key = capturedKey; lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; }
      const sentResult = result, started = now(); busy = true;
      try {
        const response = await post('/api/control-transport/bridge', {
          source_session_id: current.session, source_control_session_generation: current.generation,
          media_negotiation_id: current.attempt, ui_instance_id: current.ui, ui_control_epoch: current.uiEpoch,
          dc_ready: current.ready === true, last_seen_seq: lastSeen,
          ...(sentResult ? {vehicle_result: sentResult} : {}),
        });
        const latest = scope();
        if (!latest || scopeKey(latest) !== capturedKey || response?.protocol !== protocol) return;
        if (result === sentResult) result = null;
        negotiated = response.enabled === true && Number.isSafeInteger(response.transport?.epoch) && response.transport.epoch > 0;
        epoch = negotiated ? response.transport.epoch : 0;
        path = response.transport?.path === 'dc' ? 'dc' : 'wss';
        const nextRevision = response.transport?.revision;
        if (Number.isSafeInteger(nextRevision) && nextRevision > revision) {
          revision = nextRevision; onBoundary();
        }
        for (const status of response.statuses || []) onStatus(status);
        const frame = response.frame;
        if (!frame || !Number.isSafeInteger(frame.seq) || frame.seq <= lastSeen) return;
        lastSeen = frame.seq; // Never retry a discarded/blocked/expired frame.
        if (response.enabled !== true || !latest.ready || frame.session_id !== latest.session ||
            frame.control_transport?.protocol !== protocol || frame.control_transport?.path !== 'dc' ||
            frame.control_transport?.media_attempt !== latest.attempt ||
            typeof response.frame_json !== 'string' || response.frame_json.length > 64 * 1024 ||
            !Number.isFinite(response.valid_for_ms) || response.valid_for_ms <= 0 || now() - started >= response.valid_for_ms) return;
        send(frame, response.frame_json);
      } finally { busy = false; }
    }
    return Object.freeze({tick, receive, negotiated: () => negotiated, activePath: () => path, epoch: () => epoch});
  }
  return Object.freeze({protocol, createCourier});
});
