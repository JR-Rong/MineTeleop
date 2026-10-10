'use strict';

(function exposeCameraView(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopCameraView = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createCameraViewApi() {
  // Driving views lead the flat layout; fisheye directions keep the same order.
  const CAMERAS = Object.freeze([
    ['drive_front', '前视驾驶'], ['drive_rear', '后视驾驶'],
    ['drive_left', '左视驾驶'], ['drive_right', '右视驾驶'],
    ['fish_front', '前鱼眼'], ['fish_rear', '后鱼眼'],
    ['fish_left', '左鱼眼'], ['fish_right', '右鱼眼'], ['surround_bev','360° 鸟瞰'], ['fish_diagnostic','鱼眼四宫格 · 仅台架诊断'],
  ].map(([id, label]) => Object.freeze({id, label})));
  const STALE_MS = 3000;

  function orderedCameraIds(ids) {
    const unique = [...new Set(ids.filter(id => typeof id === 'string' && id.length))];
    const rank = id => {
      const index = CAMERAS.findIndex(camera => camera.id === id);
      return index < 0 ? CAMERAS.length : index;
    };
    return unique.sort((a, b) => rank(a) - rank(b) || (a < b ? -1 : a > b ? 1 : 0));
  }

  function cameraLabel(id) {
    const camera = CAMERAS.find(camera => camera.id === id);
    return camera ? camera.label : id;
  }

  function frameHealth(previous, {frames, now, startedAt, muted = false, ended = false, disconnected = false}) {
    const advanced = Number.isFinite(frames) && frames > 0 && frames !== previous.frames;
    const lastFrameAt = muted || ended || disconnected ? null : advanced ? now : previous.lastFrameAt;
    const stale = now - (lastFrameAt == null ? startedAt : lastFrameAt) >= STALE_MS;
    const status = ended || disconnected ? 'offline' : muted || stale ? 'stale' : lastFrameAt == null ? 'waiting' : 'live';
    return {frames, lastFrameAt, status};
  }

  function mount(grid) {
    const entries = new Map();
    let disconnected = false;
    let scope='default', descriptors=new Map(), focus=null, layout={}, worker=null, bev=null;
    let workerReady=false,workerBusy=false, pendingFrames=new Map(), generation=0;
    const frameTraces=new Map(),presentedTraces=new Map(),reportedStreams=new Map();
    function emitFrame(detail){grid.dispatchEvent(new CustomEvent('media-frame-presented',{detail}));}
    function boundFrames(map){while(map.size>256)map.delete(map.keys().next().value);}
    function noteFrameTrace(trace){if(!Number.isInteger(trace.rtp_timestamp)||!descriptors.has(trace.stream_id))return;
      const key=trace.stream_id+':'+trace.rtp_timestamp,shown=presentedTraces.get(key);
      if(shown){emitFrame({...shown,source_trace:trace,association:'rtp_exact_late_sidecar'});presentedTraces.delete(key);}
      else{frameTraces.set(key,trace);boundFrames(frameTraces);}}
    function reportFrame(id,metadata){
      metadata=metadata||{};
      const now=performance.now(),prior=reportedStreams.get(id),frames=metadata.presentedFrames;
      if(Number.isFinite(frames)&&(!prior||frames>prior.frames))reportedStreams.set(id,{at:now,frames,gap:!prior||prior.gap||now-prior.at>100});
      const key=id+':'+metadata.rtpTimestamp,trace=frameTraces.get(key);
      const detail={stream_id:id,rtp_timestamp:metadata.rtpTimestamp==null?null:metadata.rtpTimestamp,presented_frames:metadata.presentedFrames,
        media_time:metadata.mediaTime,display_monotonic_ms:performance.now(),browser_time_origin_ms:performance.timeOrigin,
        source_trace:trace||null,association:trace?'rtp_exact':'unmatched',capture_time_quality:'browser_estimate'};
      emitFrame(detail);if(trace)frameTraces.delete(key);else if(Number.isInteger(metadata.rtpTimestamp)){presentedTraces.set(key,detail);boundFrames(presentedTraces);}}
    function loadLayout(){try{const saved=JSON.parse(localStorage.getItem('mine-teleop-layout:'+scope)||'{}');layout=saved&&typeof saved==='object'&&!Array.isArray(saved)?saved:{}}catch{layout={}}}
    function saveLayout(){try{localStorage.setItem('mine-teleop-layout:'+scope,JSON.stringify(layout))}catch{}}
    function reflow(){
      grid.classList.add('camera-adjustable-grid');
      for(const [id,entry] of entries){const span=focus?(id===focus?10:2):Math.max(2,Math.min(12,Number(layout[id]&&layout[id].span)||6));
        entry.box.style.gridColumn='span '+span;entry.box.style.aspectRatio=String(entry.aspect||16/9);
        entry.box.style.order=focus&&id===focus?'-1':'';entry.box.style.maxHeight='none';
        entry.box.classList.toggle('camera-focused',id===focus);}
    }
    function enableResize(entry,id){
      entry.box.addEventListener('dblclick',event=>{if(event.target.closest('.camera-resize'))return;focus=focus===id?null:id;reflow();});
      for(const corner of ['nw','ne','sw','se']){
        const handle=document.createElement('button');handle.type='button';handle.className='camera-resize '+corner;
        handle.setAttribute('aria-label',cameraLabel(id)+' '+corner+' 调整大小');
        handle.addEventListener('pointerdown',event=>{
          event.preventDefault();event.stopPropagation();focus=null;reflow();
          const start=event.clientX,initial=entry.box.getBoundingClientRect().width,gridWidth=grid.clientWidth;
          handle.setPointerCapture(event.pointerId);
          const move=next=>{const dx=(corner.endsWith('w')?-1:1)*(next.clientX-start),dy=(corner.startsWith('n')?-1:1)*(next.clientY-event.clientY)*(entry.aspect||16/9),delta=Math.abs(dx)>=Math.abs(dy)?dx:dy;layout[id]={span:Math.max(2,Math.min(12,Math.round((initial+delta)/gridWidth*12)))};reflow();};
          const done=()=>{handle.removeEventListener('pointermove',move);handle.removeEventListener('pointerup',done);handle.removeEventListener('pointercancel',done);saveLayout();};
          handle.addEventListener('pointermove',move);handle.addEventListener('pointerup',done);handle.addEventListener('pointercancel',done);
        });
        handle.addEventListener('keydown',event=>{if(!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key))return;event.preventDefault();focus=null;layout[id]={span:Math.max(2,Math.min(12,(Number(layout[id]&&layout[id].span)||6)+(['ArrowRight','ArrowUp'].includes(event.key)?1:-1)))};reflow();saveLayout();});
        entry.box.append(handle);
      }
    }
    function sendWorkerFrames(){
      if(!worker||!workerReady||workerBusy||pendingFrames.size!==4)return;
      const frames=[...pendingFrames.values()];pendingFrames.clear();workerBusy=true;
      worker.postMessage({type:'frames',frames},frames.map(item=>item.frame));
    }
    function queueBevFrame(id,video,metadata){
      if(!worker||!id.startsWith('fish_')||!bev)return;
      // VideoFrame transfers its resource to the worker. Keep at most one
      // unconsumed frame per source and close every replacement immediately.
      const old=pendingFrames.get(id);if(old){old.frame.close();pendingFrames.delete(id);}
      if(video.readyState<2)return;
      let frame;try{frame=new VideoFrame(video,{timestamp:Math.round((metadata.captureTime||metadata.expectedDisplayTime||performance.now())*1000)});}catch{return;}

      pendingFrames.set(id,{id,frame,rtp_timestamp:metadata.rtpTimestamp==null?null:metadata.rtpTimestamp,received:performance.timeOrigin+performance.now(),aligned:performance.timeOrigin+(metadata.captureTime||metadata.expectedDisplayTime||performance.now())});sendWorkerFrames();
    }
    function startBev(calibration){
      if(!calibration||!['fish_front','fish_rear','fish_left','fish_right'].every(id=>entries.has(id)))return;
      bev=ensure('surround_bev');bev.aspect=512/896;bev.video.hidden=true;
      bev.canvas=document.createElement('canvas');bev.canvas.width=512;bev.canvas.height=896;bev.box.append(bev.canvas);
      worker=new Worker('/assets/surround_worker.js');const ownGeneration=generation;
      worker.onmessage=event=>{if(ownGeneration!==generation)return;const data=event.data;
        if(data.type==='ready'){workerReady=true;sendWorkerFrames();return;}
        if(data.type==='error'){paint(bev,'stale');bev.status.textContent='环视不可用: '+data.error;workerBusy=false;return;}
        if(data.type==='rendered'){emitFrame({stream_id:'surround_bev',association:'worker_source_rtp',inputs:data.inputs,display_monotonic_ms:performance.now(),browser_time_origin_ms:performance.timeOrigin,worker_ms:data.worker_ms,compose_ms:data.compose_ms,healthy:data.healthy});workerBusy=false;const rgba=new Uint8ClampedArray(data.rgba);bev.canvas.getContext('2d').putImageData(new ImageData(rgba,512,896),0,0);bev.health={frames:bev.health.frames+1,lastFrameAt:performance.now(),status:data.healthy?'live':'stale'};paint(bev,bev.health.status);sendWorkerFrames();}
      };
      worker.postMessage({type:'init',calibration,width:512,height:896});reorder();reflow();
    }

    function paint(entry, status) {
      if (entry.box.dataset.state === status) return;
      entry.box.dataset.state = status;
      entry.status.textContent = {waiting: '等待视频', live: '', stale: '画面中断 / 暂无新帧', offline: '视频已断开'}[status];
      entry.status.hidden = status === 'live';
      // The opaque status overlay covers stale video without pausing playback.
    }

    function ensure(id) {
      if (entries.has(id)) return entries.get(id);
      const box = document.createElement('article');
      box.className = 'camera';
      box.id = 'camera-' + id;
      box.dataset.cameraId = id;
      const label = document.createElement('span');
      label.className = 'label';
      label.textContent = cameraLabel(id);
      label.title = id;
      const video = document.createElement('video');
      video.autoplay = true;
      video.playsInline = true;
      video.muted = true;
      const status = document.createElement('div');
      status.className = 'camera-placeholder';
      status.setAttribute('role', 'status');
      box.append(label, video, status);
      const entry = {box, video, status, track: null, cleanup: null, startedAt: performance.now(), health: {frames: 0, lastFrameAt: null}};
      entries.set(id, entry);
      grid.append(box);
      enableResize(entry,id);
      reflow();
      paint(entry, 'waiting');
      return entry;
    }

    function reorder() {
      // Move existing nodes only when necessary; never recreate their videos.
      orderedCameraIds([...entries.keys()]).forEach((id, index) => {
        const box = entries.get(id).box;
        if (grid.children[index] !== box) grid.insertBefore(box, grid.children[index] || null);
      });
    }

    function reset() {
      generation++;frameTraces.clear();presentedTraces.clear();reportedStreams.clear();if(worker)worker.terminate();worker=null;workerReady=false;workerBusy=false;bev=null;
      for(const item of pendingFrames.values())item.frame.close();pendingFrames.clear();focus=null;
      for (const entry of entries.values()) {
        if (entry.cleanup) entry.cleanup();
        entry.video.srcObject = null;
      }
      entries.clear();
      grid.replaceChildren();
      disconnected = false;
    }

    function configure(ids, options={}) {
      reset();scope=String(options.vehicleId||'unknown')+':'+String(options.mode||'full');loadLayout();
      descriptors=new Map((options.streams||ids.filter(item=>typeof item==='object')).map(item=>[item.stream_id||item.camera_id,item]));
      const panels=[];
      for(const item of ids){const id=typeof item==='string'?item:item.stream_id||item.camera_id,description=descriptors.get(id);
        if(description&&description.kind==='drive_mosaic')for(const region of description.regions||[])panels.push(region.camera_id);else panels.push(id);}
      for(const id of orderedCameraIds(panels)){const entry=ensure(id);const stream=[...descriptors.values()].find(item=>(item.regions||[]).some(region=>region.camera_id===id))||descriptors.get(id);
        if(stream&&stream.kind==='surround_bev')entry.aspect=stream.width/stream.height;}
      const acceptance=options.calibration&&options.calibration.acceptance;
      reorder();reflow();if(options.mode!=='two'&&acceptance&&acceptance.profiles&&acceptance.profiles.full)startBev(options.calibration);
    }

    function presentedFrames(video) {
      const quality = video.getVideoPlaybackQuality();
      return Math.max(0, quality.totalVideoFrames - quality.droppedVideoFrames);
    }

    function refresh() {
      const now = performance.now();
      for (const entry of entries.values()) {
        if(entry===bev){if(!entry.health.lastFrameAt||now-entry.health.lastFrameAt>100||disconnected)paint(entry,'stale');continue;}
        entry.health = frameHealth(entry.health, {
          frames: entry.mosaicFrames==null?presentedFrames(entry.video):entry.mosaicFrames,
          now, startedAt: entry.startedAt,
          muted: Boolean(entry.track && entry.track.muted), ended: Boolean(entry.track && entry.track.readyState === 'ended'), disconnected,
        });
        paint(entry,entry.lastPresentedAt&&now-entry.lastPresentedAt>100?'stale':entry.health.status);
      }
    }

    function attach(id, track) {
      const description=descriptors.get(id);
      if(description&&description.kind==='drive_mosaic'){
        const video=document.createElement('video');video.muted=true;video.autoplay=true;video.playsInline=true;video.style.cssText='position:absolute;width:1px;height:1px;opacity:0;pointer-events:none';grid.append(video);video.srcObject=new MediaStream([track]);
        const panels=(description.regions||[]).map(region=>{const entry=ensure(region.camera_id);entry.video.hidden=true;entry.track=track;entry.mosaicFrames=0;entry.aspect=region.width/region.height;entry.canvas=document.createElement('canvas');entry.canvas.width=region.width;entry.canvas.height=region.height;entry.box.append(entry.canvas);return {entry,region};});
        const ownGeneration=generation;let callback=0;
        const draw=(_time,metadata)=>{if(ownGeneration!==generation)return;reportFrame(id,metadata);for(const {entry,region} of panels){entry.canvas.getContext('2d').drawImage(video,region.x,region.y,region.width,region.height,0,0,region.width,region.height);entry.mosaicFrames++;entry.lastPresentedAt=performance.now();}refresh();callback=video.requestVideoFrameCallback(draw);};
        callback=video.requestVideoFrameCallback(draw);
        panels[0].entry.cleanup=()=>{video.cancelVideoFrameCallback(callback);video.srcObject=null;video.remove();};reflow();return;
      }
      const entry = ensure(id);
      if (entry.cleanup) entry.cleanup();
      entry.track = track;
      entry.startedAt = performance.now();
      entry.video.srcObject = new MediaStream([track]);
      entry.health = {frames: presentedFrames(entry.video), lastFrameAt: null};
      paint(entry, 'waiting');
      let callback=0;const ownGeneration=generation;
      const frameReady=(_time,metadata)=>{if(ownGeneration!==generation)return;entry.lastPresentedAt=performance.now();reportFrame(id,metadata);try{queueBevFrame(id,entry.video,metadata);}finally{callback=entry.video.requestVideoFrameCallback(frameReady);}};
      if(entry.video.requestVideoFrameCallback)callback=entry.video.requestVideoFrameCallback(frameReady);
      const update = () => refresh();
      for (const event of ['mute', 'unmute', 'ended']) track.addEventListener(event, update);
      entry.cleanup = () => {
        if(callback)entry.video.cancelVideoFrameCallback(callback);
        for (const event of ['mute', 'unmute', 'ended']) track.removeEventListener(event, update);
      };
      reorder();
    }

    function setConnectionState(state) {
      disconnected = ['disconnected', 'failed', 'closed'].includes(state);
      // frameHealth requires fresh frames after transport recovery.
      refresh();
    }

    return {configure, attach, reset, refresh, setConnectionState,noteFrameTrace,
      healthyStreams(){const now=performance.now(),result=[];for(const id of descriptors.keys()){const progress=reportedStreams.get(id);if(progress&&!progress.gap&&now-progress.at<=100&&!disconnected)result.push(id);if(progress)progress.gap=false;}return result;},
      showSurround(){if(!entries.has('surround_bev'))return false;focus=focus==='surround_bev'?null:'surround_bev';reflow();return true;}};
  }

  return Object.freeze({CAMERAS, STALE_MS, orderedCameraIds, cameraLabel, frameHealth, mount});
});
