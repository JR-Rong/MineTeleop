async ({mode, seconds, transport = 'udp', networkConfig}, createNetworkPolicy, iceAddressInfo) => {
  const bridge = window.mineTeleopBench;
  const network = networkConfig ? createNetworkPolicy(networkConfig, iceAddressInfo) : null;
  const send = message => bridge.send(message).catch(error => { console.error(error); throw error; });
  const data = window.hybridBench = {schema: 'hybrid-ui-ab-v1', mode, seconds, expectedTransport: transport, userAgent: navigator.userAgent,
    expectedNetwork: networkConfig, display: {devicePixelRatio, width: screen.width, height: screen.height}, attempts: [], samples: [], tracks: [], phases: [], load: [], longTasks: [], visibility: [], errors: [], overflow: 0, start: null};
  let stopped = false, started = false, collecting = false, timer, loadTimer, nativeScope, offerSeen = false;
  let offeredIds = [], latestNative = null, lastNativeAt = 0, previousPhase = '';
  let observer, frameIds = [];
  const style = document.createElement('style');
  style.textContent = '.visual-stage .camera>video{top:32px;height:calc(100% - 32px)} #bench-load{position:fixed;right:12px;bottom:8px;width:270px;height:100px;overflow:hidden;z-index:9999;background:#102030dd;color:#cfe7fa;font:11px monospace;padding:5px;pointer-events:none}';
  document.head.append(style);
  const loadPanel = document.createElement('div'); loadPanel.id = 'bench-load'; document.body.append(loadPanel);
  const rows = Array.from({length: 100}, (_, i) => { const row = document.createElement('div'); row.textContent = `bench ${i}`; loadPanel.append(row); return row; });
  const error = value => {
    if (stopped) return;
    data.errors.push(String(value));
    if (started) stop(); else { stopped = true; void send({type: 'error', error: String(value)}); }
  };
  addEventListener('error', event => error(event.message));
  addEventListener('unhandledrejection', event => error(event.reason));
  const visibility = () => data.visibility.push({at: performance.now(), hidden: document.hidden, focused: document.hasFocus()});
  document.addEventListener('visibilitychange', visibility); addEventListener('blur', visibility); addEventListener('focus', visibility); visibility();
  // Only this opt-in, contained bench disables driving interaction. No product
  // safety readiness is synthesized from native frame counters.
  for (const id of ['estop', 'control-limits-open']) document.getElementById(id).disabled = true;
  function layout() {
    if (mode !== 'native' || !offeredIds.length || stopped) return;
    const rects = offeredIds.map(id => {
      const box = document.getElementById('camera-' + id), video = box?.querySelector('video');
      if (!video) return {x: 0, y: 0, width: 0, height: 0, visible: false};
      const r = video.getBoundingClientRect();
      const modal = Boolean(document.querySelector('dialog[open]'));
      return {x: r.x, y: r.y, width: r.width, height: r.height, visible: !document.hidden && !modal && r.width > 0 && r.height > 0};
    });
    void send({type: 'layout', rects});
  }
  if (network) {
    const originalFetch = fetchIceServers;
    fetchIceServers = async () => {
      const id=mediaOfferScope?.id, session=nativeControlSessionId, generation=nativeControlSessionGeneration;
      const servers=await originalFetch();
      // A late credential response must not overwrite the replacement peer's
      // allowlist evidence, even though the product already fences its use.
      network.requireIceServers(servers, id===mediaOfferScope?.id && session===nativeControlSessionId && generation===nativeControlSessionGeneration);
      return network.serversForReceiver(servers);
    };
  }
  if (mode === 'native') {
    // The ordinary product console owns login, session fences and signaling.
    // Only media offer/ICE handling is replaced in this injected bench script.
    collectMetrics = async () => {};
    const early = [];
    let nativeOfferReady = false;
    addIce = async candidate => {
      if (!nativeScope || !nativeOfferReady) { if (early.length >= 128) throw Error('native early ICE overflow'); early.push(candidate); return; }
      if (controlLogic.iceCandidateMatchesOffer(candidate, mediaOfferScope))
        await send({type: 'candidate', payload: {...candidate, media_negotiation_id: nativeScope.media_negotiation_id}});
    };
    startFromOffer = async offer => {
      if (started) throw Error('measurement stops on renegotiation; retain separate attempt evidence');
      if (!offer.media_negotiation_id) throw Error('native receiver requires a fenced offer');
      if (mediaOfferScope?.id===offer.media_negotiation_id || retiredMediaScopes.has('id:'+offer.media_negotiation_id)) return;
      const refreshIce=offerSeen;
      retireMediaOffer(mediaOfferScope);
      mediaOfferScope = controlLogic.iceOfferScope(offer, !offerSeen); mediaHasOffer = true; offerSeen = true;
      latestNative=null; lastNativeAt=0; nativeOfferReady=false;
      data.attempts.push({at:performance.now(),id:offer.media_negotiation_id,mode:offer.ice_route_mode||'all'});
      nativeScope = {source_session_id: nativeControlSessionId,
        source_control_session_generation: nativeControlSessionGeneration, media_negotiation_id: offer.media_negotiation_id};
      offeredIds = (offer.media_tracks || []).map(t => t.camera_id);
      if (offeredIds.length !== 2) throw Error('hybrid A/B requires exactly two video tracks');
      remoteCameraIds = offeredIds; cameraView.configure(offeredIds);
      for (const id of offeredIds) document.getElementById('camera-' + id).querySelector('video').setAttribute('aria-hidden', 'true');
      if (emptyStage.isConnected) emptyStage.remove();
      webrtcLabel.textContent = '原生视频台架 · 控制关闭';
      const scope={...nativeScope};
      const current=()=>scope.media_negotiation_id===nativeScope?.media_negotiation_id&&scope.source_session_id===nativeControlSessionId&&scope.source_control_session_generation===nativeControlSessionGeneration;
      if(refreshIce){
        let servers;
        try{servers=await fetchIceServers()}catch(error){if(!current())return;throw error}
        if(!current())return;
        iceServers=servers;
      }
      await send({type: 'offer', payload: {...offer,ice_servers:controlLogic.iceServersForMode(iceServers,offer.ice_route_mode||'all')}});
      if(!current())return;
      nativeOfferReady=true;
      for(const candidate of early.splice(0))await addIce(candidate);
    };
    bridge.onEvent(event => {
      if (stopped || event.media_negotiation_id!==nativeScope?.media_negotiation_id) return;
      const scope={...nativeScope};
      void (async () => {
        if (event.type === 'answer') {
          await post('/api/webrtc/answer', {...scope, type: 'answer', sdp: event.sdp});
          for (const candidate of early.splice(0)) await addIce(candidate);
          layout();
        } else if (event.type === 'candidate') {
          await post('/api/webrtc/ice-candidate', {...scope, candidate: event.candidate});
        } else if (event.type === 'sample') {
          latestNative = event.sample; lastNativeAt = performance.now();
          for (const id of offeredIds) {
            const placeholder = document.querySelector('#camera-' + CSS.escape(id) + ' .camera-placeholder');
            if (placeholder) placeholder.hidden = true;
          }
          if (!started && event.sample.tracks.length === 2 && event.sample.tracks.every(t => t.drawn > 30)) await start();
        }
      })().catch(e=>{if(scope.media_negotiation_id===nativeScope?.media_negotiation_id)error(e)});
    });
    addEventListener('resize', layout);
    setInterval(layout, 500);
  }
  if (network) {
    const originalOffer = startFromOffer, originalIce = addIce;
    startFromOffer = async offer => {
      if (mode==='browser') data.attempts.push({at:performance.now(),id:offer.media_negotiation_id,mode:offer.ice_route_mode||'all'});
      network.requireIceServers(iceServers);
      iceServers = network.serversForReceiver(iceServers);
      // Keep session/ufrag/negotiation fences intact while excluding other paths.
      pendingIce = pendingIce.filter(network.candidateAllowed);
      return originalOffer({...offer, sdp:network.filterSdp(offer.sdp)});
    };
    addIce = async candidate => {
      if (network.candidateAllowed(candidate)) return originalIce(candidate);
    };
  }
  function phaseAt(elapsed) {
    const ratio = elapsed / (seconds * 1000);
    return ratio < .2 ? 'idle' : ratio < .4 ? 'ui8' : ratio < .6 ? 'ui40' : ratio < .8 ? 'stall200' : 'recovery';
  }
  function load() {
    if (stopped) return;
    const begin = performance.now(), elapsed = begin - data.start, phase = phaseAt(elapsed);
    if (phase !== previousPhase) { data.phases.push({name: phase, at: begin}); previousPhase = phase; }
    const index = Math.floor(elapsed / 100);
    const work = phase === 'ui8' ? 8 : phase === 'ui40' ? 40 : phase === 'stall200' && index % 10 === 0 ? 200 : 0;
    if (work) {
      for (let i = 0; i < rows.length; i++) {
        rows[i].textContent = `${phase} #${index} row ${i} ${(index * 17 + i) % 1000}`;
        rows[i].style.color = (index + i) % 2 ? '#9ec5ef' : '#d2e4b3';
      }
      void loadPanel.offsetHeight;
      while (performance.now() - begin < work) { /* deterministic UI-thread pressure */ }
    }
    loadPanel.firstChild.textContent = `${mode} ${phase} ${(elapsed / 1000).toFixed(0)} / ${seconds}s`;
    if (data.load.length < 20000) data.load.push({at: begin, phase, requested_ms: work, actual_ms: performance.now() - begin}); else data.overflow++;
  }
  function requireTransportPairs(report) {
    const transportIds = new Set([...report.values()].filter(s => s.type === 'inbound-rtp' && s.kind === 'video').map(s => s.transportId));
    if (!transportIds.size) throw Error('missing video transport evidence');
    return [...transportIds].map(id => {
      const pair = report.get(report.get(id)?.selectedCandidatePairId);
      const local = report.get(pair?.localCandidateId), remote = report.get(pair?.remoteCandidateId);
      if (pair?.state !== 'succeeded' || local?.protocol !== transport || remote?.protocol !== transport ||
          [local,remote].some(c=>c?.candidateType==='relay' && c.relayProtocol!==transport) ||
          (!networkConfig?.allowRelay && (local.candidateType === 'relay' || remote.candidateType === 'relay')))
        throw Error(`comparison requires selected ${transport.toUpperCase()} candidates and known matching TURN legs`);
      if (network) network.requirePair({local_address:local.address,remote_address:remote.address,
        local_transport:local.protocol,remote_transport:remote.protocol,local_type:local.candidateType,remote_type:remote.candidateType,
        local_relay_protocol:local.relayProtocol,remote_relay_protocol:remote.relayProtocol,local_url:local.url,remote_url:remote.url});
      return {id: pair.id, local, remote};
    });
  }
  async function sample() {
    if (stopped || collecting) return; collecting = true;
    try {
      if (!polling || !nativeControlSessionId) throw Error('media session stopped');
      const sample = {at: performance.now(), phase: phaseAt(performance.now() - data.start), signaledTurnUrls:network?.signaledTurnUrls()};
      if (mode === 'browser') {
        if (peer !== data.instance || peer.connectionState !== 'connected') throw Error('browser peer changed/disconnected');
        const report = await peer.getStats();
        sample.selectedPairs = requireTransportPairs(report);
        sample.stats = [...report.values()].filter(s => ['inbound-rtp', 'codec', 'transport', 'candidate-pair', 'local-candidate', 'remote-candidate'].includes(s.type));
        sample.metrics = structuredClone(latestMetrics);
      } else {
        if (performance.now() - lastNativeAt > 3000) throw Error('native sample stream stopped');
        if (network) network.requirePair(latestNative.selected_pair);
        sample.native_clock_ms = latestNative.clock_ms;
        sample.tracks = latestNative.tracks;
        sample.selected_pair = latestNative.selected_pair;
      }
      if (data.samples.length < 1900) data.samples.push(sample); else data.overflow++;
    } catch (e) { error(e); }
    finally { collecting = false; }
  }
  async function start() {
    if (started) return; started = true;
    const selectedPairs = mode === 'browser' ? requireTransportPairs(await peer.getStats()) : undefined;
    if (mode === 'native' && network) network.requirePair(latestNative?.selected_pair);
    data.start = performance.now(); data.wallStart = Date.now(); data.instance = peer;
    data.nativeStart = await send({type: 'ready', at: data.start, selectedPairs});
    if (mode === 'browser') {
      for (const video of [...document.querySelectorAll('video')].filter(v => v.srcObject && v.videoWidth)) {
        const rect = video.getBoundingClientRect();
        const track = {camera: video.closest('.camera').dataset.cameraId, width: video.videoWidth, height: video.videoHeight,
          displayedWidth: rect.width, displayedHeight: rect.height, frames: []}; data.tracks.push(track);
        const frame = (now, m) => {
          if (stopped) return;
          if (track.frames.length < 60000) track.frames.push({now, presentationTime: m.presentationTime,
            expectedDisplayTime: m.expectedDisplayTime, presentedFrames: m.presentedFrames, mediaTime: m.mediaTime,
            receiveTime: m.receiveTime, rtpTimestamp: m.rtpTimestamp, processingDuration: m.processingDuration}); else data.overflow++;
          video.requestVideoFrameCallback(frame);
        };
        frameIds.push([video, video.requestVideoFrameCallback(frame)]);
      }
    }
    observer = new PerformanceObserver(entries => {
      for (const entry of entries.getEntries()) if (data.longTasks.length < 20000) data.longTasks.push([entry.startTime, entry.duration]); else data.overflow++;
    });
    observer.observe({type: 'longtask', buffered: false});
    await sample(); load(); timer = setInterval(sample, 1000); loadTimer = setInterval(load, 100);
    setTimeout(stop, seconds * 1000);
  }
  async function stop() {
    if (stopped) return; stopped = true;
    data.end = performance.now(); visibility();
    clearInterval(timer); clearInterval(loadTimer); observer?.disconnect();
    delete data.instance;
    await send({type: 'complete', data});
  }
  window.stopHybridBench = reason => { if (reason) data.errors.push(String(reason)); return stop(); };
  if (mode === 'browser') {
    const ready = setInterval(() => {
      const videos = [...document.querySelectorAll('video')].filter(v => v.srcObject && v.videoWidth && !v.paused);
      if (videos.length === 2 && peer?.connectionState === 'connected') { clearInterval(ready); void start().catch(error); }
    }, 250);
  }
  return {installed: true, mode, seconds};
}
