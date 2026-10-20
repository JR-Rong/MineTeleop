'use strict';
(function expose(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopControlTransport = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createTransport() {
  const protocol = 'native-control-transport-v1';
  const feedbackProtocol = 'native-feedback-transport-v1';
  const emergencyProtocol = 'native-stop-only-v1';
  // Read the transport carrying SCTP, not a media stats candidate pair. No
  // candidate addresses or credentials leave this function. Unknown stays WSS.
  function dataChannelRoute(peer, channel, mode = 'p2p_preferred', experimentalRelay = false) {
    let networkPath = 'unknown';
    const sctp = peer?.sctp, dtls = sctp?.transport, ice = dtls?.iceTransport;
    const open = peer?.connectionState === 'connected' && channel?.readyState === 'open';
    const connected = open && sctp?.state === 'connected' && dtls?.state === 'connected' &&
      ['connected','completed'].includes(ice?.state);
    if (connected) {
      try {
        const pair = ice.getSelectedCandidatePair();
        const types = [pair?.local?.type, pair?.remote?.type];
        if (types.includes('relay')) networkPath = 'turn_unknown';
        else if (types.every(type => ['host','srflx','prflx'].includes(type))) networkPath = 'p2p';
      } catch (_) { /* Missing/unsupported selected pair remains unknown. */ }
    }
    return {networkPath, ready: mode !== 'cloud_only' &&
      (connected && networkPath === 'p2p' || experimentalRelay && open)};
  }
  // Only this exact local marker means send was not called due to buffer
  // pressure. False/undefined, exceptions and missing ACKs never request retry.
  const emergencyBackpressure = Object.freeze({reason:'buffered_amount_above_threshold'});
  // Both the periodic fetch and receipt notifications use this single owner.
  // Notifications carry no data: the courier's existing bounded queues do.
  function createTickScheduler({tick, now, onError, schedule = callback => queueMicrotask(callback)}) {
    let running = false, queued = false, pending = null, retryAt = 0;
    function request(isCurrent = () => true) {
      if (!isCurrent()) { if (pending === isCurrent) pending = null; return; }
      pending = isCurrent;
      if (running || queued || now() < retryAt) return;
      queued = true;
      schedule(() => {
        queued = false;
        if (pending && pending()) return run();
        pending = null;
      });
    }
    async function run() {
      if (running || now() < retryAt) return;
      pending = null; running = true;
      try { await tick(); }
      catch (error) {
        // New receipts must not turn a failing HTTP endpoint into a busy loop.
        // Leave the data queued for the existing 25ms periodic fetch to retry.
        pending = null; retryAt = now() + 25;
        try { onError(error); } catch (_) {}
      } finally { running = false; if (pending) request(pending); }
    }
    return Object.freeze({request, tick:run});
  }
  function notifyPending(callback) { try { callback(); } catch (_) { /* Periodic fetch remains the fallback. */ } }
  // A stuck local fetch must not own the courier forever. Abort the actual
  // fetch as well as settling our wait; late replies never enter either lane.
  function scopedRequest(post, timeoutMs) {
    let active = null;
    return {
      cancelUnless(key) { if (active && active.key !== key) active.controller.abort(); },
      async run(path, body, key) {
        const controller = new AbortController();
        const entry = {key, controller}; active = entry;
        let abort;
        const cancelled = new Promise((_, reject) => {
          abort = () => reject(Error('control bridge request cancelled or timed out'));
          controller.signal.addEventListener('abort', abort, {once:true});
        });
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        try { return await Promise.race([post(path, body, controller.signal), cancelled]); }
        finally {
          clearTimeout(timer); controller.signal.removeEventListener('abort', abort);
          if (active === entry) active = null;
        }
      }
    };
  }
  function createEmergencyCourier({scope, post, now, send, onConfirmation = () => {}, onPending = () => {}, requestTimeoutMs = 500}) {
    const request = scopedRequest(post, requestTimeoutMs);
    let busy=false,key='',lastSeen=0,lastConfirmed=0,result=null;
    const scopeKey=value=>JSON.stringify([value.session,value.generation,value.ui,value.uiEpoch,value.attempt]);
    const notify=()=>{const captured=key;notifyPending(()=>onPending(()=>{const current=scope();return current&&scopeKey(current)===captured}))};
    function receive(message) {
      const current=scope();
      if(!current||message?.event!=='vehicle_control_result'||message.protocol!==protocol||
          message.session_id!==current.session||message.emergency_stop?.protocol!==emergencyProtocol||
          !Number.isSafeInteger(message.command_seq)||message.command_seq<=0)return false;
      if(key!==scopeKey(current)){key=scopeKey(current);lastSeen=lastConfirmed=0;result=null}
      if(!result||message.command_seq>result.command_seq){result=message;notify()}
      return true;
    }
    async function tick() {
      const current=scope();
      request.cancelUnless(current ? scopeKey(current) : '');
      if(busy)return;
      if(!current||!current.session||!current.attempt||!current.ui||current.uiEpoch<=0)return;
      const captured=scopeKey(current);
      if(key!==captured){key=captured;lastSeen=lastConfirmed=0;result=null}
      const sentResult=result,started=now();busy=true;
      let acceptedResponse=false;
      try {
        const response=await request.run('/api/control-transport/emergency',{
          source_session_id:current.session,source_control_session_generation:current.generation,
          media_negotiation_id:current.attempt,ui_instance_id:current.ui,ui_control_epoch:current.uiEpoch,
          dc_ready:current.ready===true,last_seen_seq:lastSeen,...(sentResult?{vehicle_result:sentResult}:{})},captured);
        const latest=scope();
        if(!latest||scopeKey(latest)!==captured||response?.protocol!==emergencyProtocol)return;
        acceptedResponse=true;
        if(result===sentResult)result=null;
        if(response.enabled===true&&response.software_latched_confirmed===true&&response.physical_execution_confirmed===false&&response.covers_all_commands===false&&
            Number.isSafeInteger(response.confirmed_command_seq)&&response.confirmed_command_seq>lastConfirmed){
          lastConfirmed=response.confirmed_command_seq;onConfirmation(lastConfirmed);
        }
        const frame=response.frame;
        if(!frame||!Number.isSafeInteger(frame.seq)||frame.seq<=lastSeen)return;
        const previousSeen=lastSeen;
        lastSeen=frame.seq; // Discard by default, including invalid/expired frames and send errors.
        if(response.enabled!==true||!latest.ready||frame.session_id!==latest.session||frame.estop!==true||
            frame.throttle!==0||frame.steering!==0||frame.brake!==1||frame.emergency_stop?.protocol!==emergencyProtocol||
            frame.emergency_stop.media_attempt!==latest.attempt||typeof response.frame_json!=='string'||response.frame_json.length>64*1024||
            !Number.isFinite(response.valid_for_ms)||response.valid_for_ms<=0||now()-started>=response.valid_for_ms)return;
        if(send(response.frame_json)===emergencyBackpressure)lastSeen=previousSeen;
        // Refetch only the current native publication on the existing tick;
        // never cache bytes, regenerate a timestamp or retry an enqueued frame.
      } finally {
        busy=false;
        const latest=scope();
        if(acceptedResponse&&result&&latest&&scopeKey(latest)===captured)notify();
      }
    }
    return Object.freeze({tick,receive});
  }
  function createCourier({scope, post, now, send, onBoundary, onStatus = () => {}, onTrace = () => {}, onPending = () => {}, requestTimeoutMs = 500}) {
    const request = scopedRequest(post, requestTimeoutMs);
    let busy = false, key = '', lastSeen = 0, revision = 0, negotiated = false, path = 'wss', epoch = 0, feedback = {};
    // Commands remain latest-only. Execution results are independent receipts:
    // replacing one loses evidence and can conceal a rejection between accepts.
    const results = [], resultCapacity = 64;
    // Diagnostic clocks are local observations, never evidence for freshness or
    // authority. Keep a failing trace sink outside the control decision path.
    const trace = value => { try { onTrace(value); } catch (_) {} };
    const statuses = new Map();
    const scopeKey = value => JSON.stringify([value.session, value.generation, value.ui, value.uiEpoch, value.attempt]);
    const notify = () => {
      const captured = key;
      notifyPending(() => onPending(() => { const current = scope(); return current && scopeKey(current) === captured; }));
    };
    function receive(message) {
      const current = scope();
      if (current && key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; results.length = 0; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
      if (current && message?.protocol === protocol && message.event === 'vehicle_control_status') {
        const stamp = message.feedback_transport, kind = message.status?.event;
        if (message.session_id !== current.session || stamp?.protocol !== feedbackProtocol || stamp.media_attempt !== current.attempt ||
            !Number.isSafeInteger(stamp.epoch) || stamp.epoch <= 0 || stamp.epoch < (feedback.epoch || 0) || stamp.epoch > (feedback.epoch || 0) + 1 ||
            !['vehicle_telemetry','vcu_handshake_status','session_control_profile_status','control_command_rejected'].includes(kind) ||
            !Number.isSafeInteger(message.status.control_status_seq) || message.status.control_status_seq <= 0) return false;
        if (key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; results.length = 0; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
        const old = statuses.get(kind);
        if (!old || stamp.epoch > old.feedback_transport.epoch || (stamp.epoch === old.feedback_transport.epoch && message.status.control_status_seq > old.status.control_status_seq)) {
          statuses.set(kind, message); notify();
        }
        return true;
      }
      if (!current || message?.protocol !== protocol || message.event !== 'vehicle_control_result' ||
          message.session_id !== current.session || !Number.isSafeInteger(message.command_seq) || message.command_seq <= 0 ||
          !Number.isSafeInteger(message.result_seq) || message.result_seq <= 0) return false;
      if (key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; results.length = 0; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
      if (message.emergency_stop) return false;
      const explicitZero = value => value?.feedback_transport?.epoch === 0 && value.feedback_transport.state === 'uninitialized' &&
        value.feedback_transport.protocol === feedbackProtocol && value.feedback_transport.from_epoch === 0 && value.feedback_transport.path === 'wss' &&
        value.feedback_request?.protocol === feedbackProtocol && value.feedback_request.media_attempt === current.attempt &&
        value.feedback_transport.media_attempt === current.attempt && value.feedback_transport.authority_issuer === value.feedback_request.authority_issuer &&
        value.feedback_transport.authority_epoch === value.feedback_request.authority_epoch;
      const feedbackEpoch = value => explicitZero(value) ? value.feedback_request.epoch : value?.feedback_transport?.epoch || 0;
      if (feedback.enabled === true && explicitZero(message) && (feedback.selecting !== true || feedbackEpoch(message) !== feedback.epoch)) return false;
      if (feedback.enabled === true && (!Number.isSafeInteger(feedbackEpoch(message)) || feedbackEpoch(message) < feedback.epoch ||
          feedbackEpoch(message) > feedback.epoch + 1 || message.feedback_transport?.media_attempt !== current.attempt)) return false;
      const receivedAt = now();
      const duplicate = results.some(entry => feedbackEpoch(entry.message) === feedbackEpoch(message) &&
        entry.message.result_seq === message.result_seq);
      const selected = !duplicate && results.length < resultCapacity;
      trace({stage:'dc_result_browser_received',session_id:current.session,seq:message.command_seq,
        result_seq:message.result_seq,received_monotonic_ms:receivedAt,mailbox_selected:selected,
        superseded_result_seq:null,queue_depth:results.length});
      if (!duplicate && !selected) {
        trace({stage:'dc_result_queue_overflow',session_id:current.session,seq:message.command_seq,
          result_seq:message.result_seq,queue_capacity:resultCapacity,queue_depth:results.length});
        return false;
      }
      if (selected) { results.push({message,receivedAt}); notify(); }
      return true;
    }
    async function tick() {
      const current = scope();
      request.cancelUnless(current ? scopeKey(current) : '');
      if (busy) return;
      if (!current || !current.session || !current.attempt || !current.ui || current.uiEpoch <= 0) return;
      const capturedKey = scopeKey(current);
      if (key !== capturedKey) { key = capturedKey; lastSeen = revision = 0; results.length = 0; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
      const sentEntry = results[0], sentResult = sentEntry?.message, sentResultReceivedAt = sentEntry?.receivedAt,
        sentStatuses = [...statuses.values()], started = now(); busy = true;
      let completed = false, acceptedResponse = false;
      try {
        const response = await request.run('/api/control-transport/bridge', {
          source_session_id: current.session, source_control_session_generation: current.generation,
          media_negotiation_id: current.attempt, ui_instance_id: current.ui, ui_control_epoch: current.uiEpoch,
          dc_ready: current.ready === true, dc_network_path: current.networkPath || "unknown", last_seen_seq: lastSeen,
          ...(sentResult ? {vehicle_result: sentResult} : {}),
          ...(sentStatuses.length ? {vehicle_statuses: sentStatuses} : {}),
        }, capturedKey);
        completed = true;
        const latest = scope();
        if (!latest || scopeKey(latest) !== capturedKey || response?.protocol !== protocol) return;
        acceptedResponse = true;
        if (sentEntry && results[0] === sentEntry) results.shift();
        for (const status of sentStatuses) if (statuses.get(status.status.event) === status) statuses.delete(status.status.event);
        feedback = response.feedback_transport || {};
        negotiated = response.enabled === true && Number.isSafeInteger(response.transport?.epoch) && response.transport.epoch > 0;
        epoch = negotiated ? response.transport.epoch : 0;
        path = response.transport?.path === 'dc' ? 'dc' : 'wss';
        const nextRevision = response.transport?.revision;
        if (Number.isSafeInteger(nextRevision) && nextRevision > revision) {
          revision = nextRevision; onBoundary();
        }
        for (const status of response.statuses || []) {
          const stamp=status?.feedback_transport;
          if(feedback.enabled===true&&(!stamp||stamp.protocol!==feedbackProtocol||stamp.epoch!==feedback.epoch||
              stamp.authority_epoch!==feedback.authority_epoch||stamp.authority_issuer!==feedback.authority_issuer||
              stamp.media_attempt!==feedback.media_attempt||stamp.path!==feedback.path||stamp.status_floor!==feedback.status_floor))continue;
          onStatus(status);
        }
        const frame = response.frame;
        if (!frame || !Number.isSafeInteger(frame.seq) || frame.seq <= lastSeen) return;
        lastSeen = frame.seq; // Never retry a discarded/blocked/expired frame.
        if (response.enabled !== true || !latest.ready || frame.session_id !== latest.session ||
            frame.control_transport?.protocol !== protocol || frame.control_transport?.path !== 'dc' ||
            frame.control_transport?.media_attempt !== latest.attempt ||
            typeof response.frame_json !== 'string' || response.frame_json.length > 64 * 1024 ||
            !Number.isFinite(response.valid_for_ms) || response.valid_for_ms <= 0 || now() - started >= response.valid_for_ms) {
          trace({stage:'dc_command_not_sent',session_id:current.session,seq:frame.seq,
            reason:'frame_ineligible_or_expired',browser_bridge_elapsed_ms:Math.max(0,now()-started)});
          return;
        }
        try {
          const outcome = send(frame, response.frame_json);
          trace({stage:outcome === true ? 'dc_command_sent' : 'dc_command_not_sent',session_id:current.session,
            seq:frame.seq,intent_seq:frame.intent_seq,epoch:frame.control_transport.epoch,
            reason:outcome === true ? 'data_channel_send_returned' : outcome?.reason || 'send_not_confirmed',
            browser_sent_monotonic_ms:now()});
        } catch (error) {
          trace({stage:'dc_command_send_failed',session_id:current.session,seq:frame.seq,reason:'data_channel_send_threw'});
          throw error;
        }
      } finally {
        busy = false;
        if (sentResult) trace({stage:'dc_result_bridge_completed',session_id:current.session,
          seq:sentResult.command_seq,result_seq:sentResult.result_seq,completed,
          browser_received_monotonic_ms:sentResultReceivedAt,post_started_monotonic_ms:started,
          browser_mailbox_wait_ms:Math.max(0,started-sentResultReceivedAt),
          bridge_http_elapsed_ms:Math.max(0,now()-started)});
        const latest = scope();
        if (acceptedResponse && (results.length || statuses.size) && latest && scopeKey(latest) === capturedKey)
          notify();
      }
    }
    return Object.freeze({tick, receive, negotiated: () => negotiated, activePath: () => path, epoch: () => epoch, feedback: () => feedback});
  }
  return Object.freeze({protocol, feedbackProtocol, emergencyProtocol, emergencyBackpressure, dataChannelRoute, createTickScheduler, createCourier, createEmergencyCourier});
});
