'use strict';
(function expose(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopControlTransport = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createTransport() {
  const protocol = 'native-control-transport-v1';
  const feedbackProtocol = 'native-feedback-transport-v1';
  const emergencyProtocol = 'native-stop-only-v1';
  function createEmergencyCourier({scope, post, now, send, onConfirmation = () => {}}) {
    let busy=false,key='',lastSeen=0,lastConfirmed=0,result=null;
    const scopeKey=value=>JSON.stringify([value.session,value.generation,value.ui,value.uiEpoch,value.attempt]);
    function receive(message) {
      const current=scope();
      if(!current||message?.event!=='vehicle_control_result'||message.protocol!==protocol||
          message.session_id!==current.session||message.emergency_stop?.protocol!==emergencyProtocol||
          !Number.isSafeInteger(message.command_seq)||message.command_seq<=0)return false;
      if(key!==scopeKey(current)){key=scopeKey(current);lastSeen=lastConfirmed=0;result=null}
      if(!result||message.command_seq>result.command_seq)result=message;
      return true;
    }
    async function tick() {
      if(busy)return;
      const current=scope();
      if(!current||!current.session||!current.attempt||!current.ui||current.uiEpoch<=0)return;
      const captured=scopeKey(current);
      if(key!==captured){key=captured;lastSeen=lastConfirmed=0;result=null}
      const sentResult=result,started=now();busy=true;
      try {
        const response=await post('/api/control-transport/emergency',{
          source_session_id:current.session,source_control_session_generation:current.generation,
          media_negotiation_id:current.attempt,ui_instance_id:current.ui,ui_control_epoch:current.uiEpoch,
          dc_ready:current.ready===true,last_seen_seq:lastSeen,...(sentResult?{vehicle_result:sentResult}:{})});
        const latest=scope();
        if(!latest||scopeKey(latest)!==captured||response?.protocol!==emergencyProtocol)return;
        if(result===sentResult)result=null;
        if(response.enabled===true&&response.software_latched_confirmed===true&&response.physical_execution_confirmed===false&&response.covers_all_commands===false&&
            Number.isSafeInteger(response.confirmed_command_seq)&&response.confirmed_command_seq>lastConfirmed){
          lastConfirmed=response.confirmed_command_seq;onConfirmation(lastConfirmed);
        }
        const frame=response.frame;
        if(!frame||!Number.isSafeInteger(frame.seq)||frame.seq<=lastSeen)return;
        lastSeen=frame.seq; // No catch-up, regeneration or cross-epoch retry.
        if(response.enabled!==true||!latest.ready||frame.session_id!==latest.session||frame.estop!==true||
            frame.throttle!==0||frame.steering!==0||frame.brake!==1||frame.emergency_stop?.protocol!==emergencyProtocol||
            frame.emergency_stop.media_attempt!==latest.attempt||typeof response.frame_json!=='string'||response.frame_json.length>64*1024||
            !Number.isFinite(response.valid_for_ms)||response.valid_for_ms<=0||now()-started>=response.valid_for_ms)return;
        send(response.frame_json);
      } finally {busy=false}
    }
    return Object.freeze({tick,receive});
  }
  function createCourier({scope, post, now, send, onBoundary, onStatus = () => {}}) {
    let busy = false, key = '', lastSeen = 0, revision = 0, result = null, negotiated = false, path = 'wss', epoch = 0, feedback = {};
    const statuses = new Map();
    const scopeKey = value => JSON.stringify([value.session, value.generation, value.ui, value.uiEpoch, value.attempt]);
    function receive(message) {
      const current = scope();
      if (current && key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
      if (current && message?.protocol === protocol && message.event === 'vehicle_control_status') {
        const stamp = message.feedback_transport, kind = message.status?.event;
        if (message.session_id !== current.session || stamp?.protocol !== feedbackProtocol || stamp.media_attempt !== current.attempt ||
            !Number.isSafeInteger(stamp.epoch) || stamp.epoch <= 0 || stamp.epoch < (feedback.epoch || 0) || stamp.epoch > (feedback.epoch || 0) + 1 ||
            !['vehicle_telemetry','vcu_handshake_status','session_control_profile_status','control_command_rejected'].includes(kind) ||
            !Number.isSafeInteger(message.status.control_status_seq) || message.status.control_status_seq <= 0) return false;
        if (key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
        const old = statuses.get(kind);
        if (!old || stamp.epoch > old.feedback_transport.epoch || (stamp.epoch === old.feedback_transport.epoch && message.status.control_status_seq > old.status.control_status_seq)) statuses.set(kind, message);
        return true;
      }
      if (!current || message?.protocol !== protocol || message.event !== 'vehicle_control_result' ||
          message.session_id !== current.session || !Number.isSafeInteger(message.command_seq) || message.command_seq <= 0 ||
          !Number.isSafeInteger(message.result_seq) || message.result_seq <= 0) return false;
      if (key !== scopeKey(current)) { key = scopeKey(current); lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
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
      if (!result || feedbackEpoch(message) > feedbackEpoch(result) ||
          (feedbackEpoch(message) === feedbackEpoch(result) && message.result_seq > result.result_seq)) result = message;
      return true;
    }
    async function tick() {
      if (busy) return;
      const current = scope();
      if (!current || !current.session || !current.attempt || !current.ui || current.uiEpoch <= 0) return;
      const capturedKey = scopeKey(current);
      if (key !== capturedKey) { key = capturedKey; lastSeen = revision = 0; result = null; negotiated = false; epoch = 0; feedback = {}; statuses.clear(); }
      const sentResult = result, sentStatuses = [...statuses.values()], started = now(); busy = true;
      try {
        const response = await post('/api/control-transport/bridge', {
          source_session_id: current.session, source_control_session_generation: current.generation,
          media_negotiation_id: current.attempt, ui_instance_id: current.ui, ui_control_epoch: current.uiEpoch,
          dc_ready: current.ready === true, dc_network_path: current.networkPath || "unknown", last_seen_seq: lastSeen,
          ...(sentResult ? {vehicle_result: sentResult} : {}),
          ...(sentStatuses.length ? {vehicle_statuses: sentStatuses} : {}),
        });
        const latest = scope();
        if (!latest || scopeKey(latest) !== capturedKey || response?.protocol !== protocol) return;
        if (result === sentResult) result = null;
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
            !Number.isFinite(response.valid_for_ms) || response.valid_for_ms <= 0 || now() - started >= response.valid_for_ms) return;
        send(frame, response.frame_json);
      } finally { busy = false; }
    }
    return Object.freeze({tick, receive, negotiated: () => negotiated, activePath: () => path, epoch: () => epoch, feedback: () => feedback});
  }
  return Object.freeze({protocol, feedbackProtocol, emergencyProtocol, createCourier, createEmergencyCourier});
});
