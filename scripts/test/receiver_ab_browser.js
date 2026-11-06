// Paste/evaluate this function in the product console after both videos play.
// Read-only bench instrumentation: no control input, ICE or buffer overrides.
// Export window.receiverAB after stop(); reload before the next independent run.
() => {
  if (window.receiverAB) throw Error('reload before another receiver measurement');
  const videos = [...document.querySelectorAll('video')].filter(v => v.srcObject && v.videoWidth);
  if (videos.length !== 2 || !peer || peer.connectionState !== 'connected') throw Error('two connected videos required');
  const instance = peer;
  const data = window.receiverAB = {
    schema: 'browser-receiver-ab-v1', start: performance.now(), wallStart: Date.now(),
    userAgent: navigator.userAgent, samples: [], tracks: [], visibility: [], longTasks: [], errors: [],
  };
  let stopped = false, collecting = false;
  const visibility = () => data.visibility.push({at: performance.now(), hidden: document.hidden, focused: document.hasFocus()});
  visibility();
  document.addEventListener('visibilitychange', visibility);
  const observer = new PerformanceObserver(entries => {
    for (const e of entries.getEntries()) if (data.longTasks.length < 10000) data.longTasks.push([e.startTime, e.duration]);
  });
  observer.observe({type: 'longtask', buffered: false});
  for (const video of videos) {
    const rect = video.getBoundingClientRect();
    const track = {id: video.id, streamId: video.srcObject.id, width: video.videoWidth, height: video.videoHeight,
      displayedWidth: rect.width, displayedHeight: rect.height, frames: []};
    data.tracks.push(track);
    const frame = (now, m) => {
      if (stopped) return;
      if (track.frames.length < 18000) track.frames.push({now, presentationTime: m.presentationTime,
        expectedDisplayTime: m.expectedDisplayTime, mediaTime: m.mediaTime, presentedFrames: m.presentedFrames,
        processingDuration: m.processingDuration, receiveTime: m.receiveTime, rtpTimestamp: m.rtpTimestamp});
      video.requestVideoFrameCallback(frame);
    };
    video.requestVideoFrameCallback(frame);
  }
  const timer = setInterval(async () => {
    if (stopped || collecting) return;
    collecting = true;
    try {
      if (peer !== instance || instance.connectionState !== 'connected') throw Error('measurement session changed or disconnected');
      const report = await instance.getStats();
      if (!stopped && data.samples.length < 300) data.samples.push({at: performance.now(),
        metrics: structuredClone(latestMetrics), stats: [...report.values()].filter(s =>
          ['inbound-rtp','codec','transport','candidate-pair','local-candidate','remote-candidate'].includes(s.type))});
    } catch (e) { data.errors.push(String(e)); data.stop(); }
    finally { collecting = false; }
  }, 1000);
  data.stop = () => {
    if (stopped) return;
    stopped = true; clearInterval(timer); clearTimeout(timeout); observer.disconnect();
    visibility(); document.removeEventListener('visibilitychange', visibility); data.end = performance.now();
  };
  const timeout = setTimeout(data.stop, 60000);
  return {started: true, tracks: videos.length};
}
