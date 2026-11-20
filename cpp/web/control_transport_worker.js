'use strict';
(function expose(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopControlWorker = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createWorkerAdapter() {
  const scopeKey = value => value ? JSON.stringify([value.session,value.generation,value.ui,value.uiEpoch,value.attempt]) : '';

  // No input sampling, signing or timestamp renewal is allowed here. The
  // existing native input lease and the original bridge/vehicle expiry remain
  // authoritative even while the page is busy rendering.
  function workerMain(transport) {
    let channel = null, snapshot = null, owner = 0, origin = '', tracePending = false, viewPending = false;
    let traceSequence = 0, droppedTraces = 0;
    const traces = [];
    const epochNow = () => performance.timeOrigin + performance.now();
    const key = value => value ? JSON.stringify([value.session,value.generation,value.ui,value.uiEpoch,value.attempt]) : '';
    const emit = message => self.postMessage({owner,...message});
    function scope(kind) {
      if (!snapshot || !Number.isFinite(snapshot.published)) return null;
      // The page and this worker keep separate performance clocks, so a snapshot
      // published a few milliseconds ahead of the worker clock is ordinary jitter,
      // not a stale page. The A50 probe recorded 3,168 of 180,800 scope reads
      // (1.75%, about 9 per second) rejected only by that ordering check, and each
      // one cancelled the in-flight bridge request. Only a genuinely late page, or
      // a timestamp implausibly far ahead, invalidates the scope.
      const age = epochNow() - snapshot.published;
      if (age < -1000 || age >= 200) return null;
      const value = snapshot[kind];
      return value ? {...value,ready:value.ready === true && channel?.readyState === 'open'} : null;
    }
    async function post(path, body, signal) {
      const response = await fetch(origin + path, {
        method:'POST',headers:{'content-type':'application/json'},
        credentials:'same-origin',body:JSON.stringify(body),signal,
      });
      const value = await response.json();
      if (!response.ok) throw Object.assign(Error(value.error || 'control bridge rejected request'), {status:response.status});
      return value;
    }
    function flushTrace() {
      if (tracePending || (!traces.length && !droppedTraces)) return;
      tracePending = true;
      emit({type:'trace',sequence:++traceSequence,records:traces.splice(0),dropped:droppedTraces});
      droppedTraces = 0;
    }
    function trace(value) {
      if (traces.length < 64) traces.push({...value,execution_context:'dedicated_worker',
        worker_event_at_utc_ms:Date.now(),worker_time_origin_ms:performance.timeOrigin});
      else ++droppedTraces;
    }
    let controlScheduler, emergencyScheduler, waitingForDrain = false;
    const control = transport.createCourier({
      scope:() => scope('control'),post,now:() => performance.now(),
      canSend:() => {
        if (channel?.readyState !== 'open') return false;
        const empty = Number(channel.bufferedAmount) === 0;
        waitingForDrain = !empty;
        return empty;
      },
      send:(_frame, bytes) => {
        if (channel?.readyState !== 'open') return {reason:'channel_not_open'};
        if (Number(channel.bufferedAmount) !== 0) return {reason:'channel_buffer_not_empty'};
        channel.send(bytes); return true;
      },
      onBoundary:() => emit({type:'boundary',scope:key(scope('control'))}),
      onStatus:value => emit({type:'status',scope:key(scope('control')),value}),
      onTrace:trace,onPending:current => controlScheduler.request(current),
    });
    const emergency = transport.createEmergencyCourier({
      scope:() => scope('emergency'),post,now:() => performance.now(),
      send:bytes => {
        if (channel?.readyState !== 'open') return;
        const buffered = Number(channel.bufferedAmount), threshold = Number(channel.bufferedAmountLowThreshold);
        if (!Number.isFinite(buffered) || buffered < 0 || !Number.isFinite(threshold) || threshold < 0) return;
        if (buffered > threshold) return transport.emergencyBackpressure;
        channel.send(bytes); return true;
      },
      onConfirmation:seq => emit({type:'confirmation',scope:key(scope('emergency')),seq}),
      onPending:current => emergencyScheduler.request(current),
    });
    controlScheduler = transport.createTickScheduler({
      tick:() => control.tick(),beforeTick:() => control.cancelStaleRequest(),now:() => performance.now(),
      onError:error => emit({type:'error',lane:'control',message:error.message}),
    });
    emergencyScheduler = transport.createTickScheduler({
      tick:() => emergency.tick(),beforeTick:() => emergency.cancelStaleRequest(),now:() => performance.now(),
      onError:error => emit({type:'error',lane:'emergency',message:error.message}),
    });
    function receive(data) {
      if (typeof data !== 'string' || data.length > 64 * 1024) return;
      try {
        const message = JSON.parse(data);
        if (message.event === 'vehicle_control_result') {
          if (message.emergency_stop) emergency.receive(message);
          control.receive(message);
        } else if (message.event === 'vehicle_control_status') control.receive(message);
        else emit({type:'message',data});
      } catch (_) { emit({type:'error',lane:'channel',message:'Invalid control channel message'}); }
    }
    self.onmessage = event => {
      const message = event.data;
      if (message?.type === 'init' && !channel) {
        const url = new URL(message.origin);
        if (url.origin !== self.location.origin &&
            url.origin !== self.origin) throw Error('Worker bridge origin mismatch');
        origin = url.origin; owner = message.owner; channel = message.channel;
        channel.onopen = () => emit({type:'channel',state:'open'});
        channel.onclose = () => emit({type:'channel',state:'closed'});
        channel.onerror = () => emit({type:'channel',state:'error'});
        channel.onmessage = event => receive(event.data);
        channel.onbufferedamountlow = () => {
          if (Number(channel?.bufferedAmount) === 0) waitingForDrain = false;
          controlScheduler.request(() => Boolean(scope('control')));
          emergencyScheduler.request(() => Boolean(scope('emergency')));
        };
        channel.bufferedAmountLowThreshold = 1024;
        emit({type:'channel',state:channel.readyState});
        emit({type:'ready'});
      } else if (message?.type === 'scope' && message.owner === owner) {
        // The sender's monotonic publication time, never receipt time.
        snapshot = message.snapshot;
        control.cancelStaleRequest(); emergency.cancelStaleRequest();
      } else if (message?.type === 'trace_ack' && message.owner === owner &&
                 message.sequence === traceSequence) {
        tracePending = false;
      } else if (message?.type === 'view_ack' && message.owner === owner) {
        viewPending = false;
      } else if (message?.type === 'threshold' && message.owner === owner && channel) {
        channel.bufferedAmountLowThreshold = message.value;
      } else if (message?.type === 'close' && message.owner === owner) channel?.close();
    };
    setInterval(() => {
      if (waitingForDrain && channel?.readyState === 'open' && Number(channel.bufferedAmount) === 0) {
        waitingForDrain = false;
        controlScheduler.request(() => Boolean(scope('control')));
      }
    },5);
    setInterval(() => { controlScheduler.tick(); emergencyScheduler.tick(); },25);
    setInterval(() => {
      if (!channel) return;
      if (!viewPending) {
        viewPending = true;
        emit({type:'view',scope:key(scope('control')),negotiated:control.negotiated(),
          path:control.activePath(),epoch:control.epoch(),feedback:control.feedback(),
          buffered:Number(channel.bufferedAmount)});
      }
      flushTrace();
    },100);
  }

  function create({transport, enabled = true, WorkerClass = globalThis.Worker,
                   onMode = () => {}, onError = () => {},
                   epochNow = () => performance.timeOrigin + performance.now()}) {
    let controlOptions, emergencyOptions, legacyControl, legacyEmergency, actor = null, sequence = 0;
    let view = {negotiated:false,path:'wss',epoch:0,feedback:{}};
    const reportMode = value => { try { onMode(value); } catch (_) {} };
    const reportError = value => { try { onError(value); } catch (_) {} };
    function scopes() {
      return {published:epochNow(),control:controlOptions?.scope() || null,emergency:emergencyOptions?.scope() || null};
    }
    function publish(force = false) {
      if (!actor) return;
      const snapshot = scopes();
      const fingerprint = JSON.stringify([snapshot.control,snapshot.emergency]);
      if (force || actor.fingerprint !== fingerprint) {
        actor.fingerprint = fingerprint;
        actor.worker.postMessage({type:'scope',owner:actor.owner,snapshot});
      }
    }
    function retire() {
      const old = actor;
      actor = null;
      if (!old) return;
      clearInterval(old.timer);
      old.worker.terminate();
      if (old.url) URL.revokeObjectURL(old.url);
    }
    function createCourier(options) {
      controlOptions = options;
      legacyControl = transport.createCourier({...options,scope:() => actor ? null : options.scope()});
      return Object.freeze({
        tick:() => actor ? undefined : legacyControl.tick(),
        receive:value => actor ? false : legacyControl.receive(value),
        cancelStaleRequest:() => actor ? publish() : legacyControl.cancelStaleRequest(),
        negotiated:() => actor ? view.negotiated : legacyControl.negotiated(),
        activePath:() => actor ? view.path : legacyControl.activePath(),
        epoch:() => actor ? view.epoch : legacyControl.epoch(),
        feedback:() => actor ? view.feedback : legacyControl.feedback(),
      });
    }
    function createEmergencyCourier(options) {
      emergencyOptions = options;
      legacyEmergency = transport.createEmergencyCourier({...options,scope:() => actor ? null : options.scope()});
      return Object.freeze({
        tick:() => actor ? undefined : legacyEmergency.tick(),
        receive:value => actor ? false : legacyEmergency.receive(value),
        cancelStaleRequest:() => actor ? publish() : legacyEmergency.cancelStaleRequest(),
      });
    }
    function adopt(raw) {
      retire();
      if (!enabled || typeof WorkerClass !== 'function' || typeof transport.factorySource !== 'function') {
        reportMode({context:'page',reason:'worker_unavailable'}); return raw;
      }
      let worker, url;
      const owner = ++sequence;
      const facade = {
        label:raw.label,protocol:raw.protocol,ordered:raw.ordered,maxRetransmits:raw.maxRetransmits,
        id:raw.id,readyState:'connecting',bufferedAmount:0,onopen:null,onmessage:null,onclose:null,onerror:null,
        _threshold:1024,
        get bufferedAmountLowThreshold() { return this._threshold; },
        set bufferedAmountLowThreshold(value) {
          this._threshold = value;
          if (actor?.owner === owner) actor.worker.postMessage({type:'threshold',owner,value});
        },
        close() { if (actor?.owner === owner) actor.worker.postMessage({type:'close',owner}); },
        // Only the original native courier in the worker may send wire bytes.
        send() { throw Error('Control channel is owned by its worker courier'); },
      };
      try {
        url = URL.createObjectURL(new Blob(['(' + workerMain.toString() + ')(' + transport.factorySource() + ');'], {type:'application/javascript'}));
        worker = new WorkerClass(url);
        worker.onmessage = event => {
          const message = event.data;
          if (!actor || actor.owner !== owner || message?.owner !== owner) return;
          if (message.type === 'ready') {
            if (actor.url) { URL.revokeObjectURL(actor.url); actor.url = null; }
            publish(true);
          } else if (message.type === 'channel') {
            if (message.state === 'error') facade.onerror?.({});
            else {
              const previous = facade.readyState;
              facade.readyState = message.state;
              if (previous !== message.state) {
                if (message.state === 'open') facade.onopen?.({});
                if (message.state === 'closed') facade.onclose?.({});
              }
              publish(true);
            }
          } else if (message.type === 'message') facade.onmessage?.({data:message.data});
          else if (message.type === 'trace') {
            try {
              for (const record of message.records || []) {
                try { controlOptions?.onTrace?.(record); } catch (_) {}
              }
              if (message.dropped) reportMode({context:'worker',trace_records_dropped:message.dropped});
            } finally { worker.postMessage({type:'trace_ack',owner,sequence:message.sequence}); }
          } else if (message.type === 'error') reportError({lane:message.lane,message:message.message});
          else if (message.type === 'view') {
            if (message.scope === scopeKey(controlOptions?.scope())) {
              view = {negotiated:message.negotiated,path:message.path,epoch:message.epoch,feedback:message.feedback};
              facade.bufferedAmount = message.buffered;
            }
            worker.postMessage({type:'view_ack',owner});
          } else if (message.type === 'boundary' && message.scope === scopeKey(controlOptions?.scope())) controlOptions?.onBoundary();
          else if (message.type === 'status' && message.scope === scopeKey(controlOptions?.scope())) controlOptions?.onStatus?.(message.value);
          else if (message.type === 'confirmation' && message.scope === scopeKey(emergencyOptions?.scope())) emergencyOptions?.onConfirmation?.(message.seq);
        };
        worker.onerror = () => {
          if (actor?.owner !== owner) return;
          facade.readyState = 'closed';
          try { facade.onerror?.({}); } catch (_) {}
          try { facade.onclose?.({}); } finally { retire(); }
          reportError({lane:'worker',message:'Control courier worker stopped'});
        };
        // Transfer synchronously in ondatachannel, before the browser's next
        // networking task makes the RTCDataChannel non-transferable.
        worker.postMessage({type:'init',owner,origin:location.origin,channel:raw},[raw]);
      } catch (error) {
        worker?.terminate();
        if (url) URL.revokeObjectURL(url);
        reportMode({context:'page',reason:error.name || 'worker_transfer_failed'});
        return raw;
      }
      actor = {owner,worker,url,timer:null,fingerprint:''};
      view = {negotiated:false,path:'wss',epoch:0,feedback:{}};
      legacyControl?.cancelStaleRequest(); legacyEmergency?.cancelStaleRequest();
      actor.timer = setInterval(() => publish(true),50);
      reportMode({context:'worker'});
      return facade;
    }
    return Object.freeze({adopt,createCourier,createEmergencyCourier,retire});
  }
  return Object.freeze({create,workerMain});
});
