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
    ['fish_left', '左鱼眼'], ['fish_right', '右鱼眼'],
  ].map(([id, label]) => Object.freeze({id, label})));
  const STALE_MS = 3000;
  const CRITICAL_DISPLAY_STALE_MS = 1000;

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

  function frameHealth(previous, {frames, now, startedAt, muted = false, ended = false, disconnected = false, staleMs = STALE_MS}) {
    const advanced = Number.isFinite(frames) && frames > 0 && frames !== previous.frames;
    const lastFrameAt = muted || ended || disconnected ? null : advanced ? now : previous.lastFrameAt;
    const stale = now - (lastFrameAt == null ? startedAt : lastFrameAt) >= staleMs;
    const status = ended || disconnected ? 'offline' : muted || stale ? 'stale' : lastFrameAt == null ? 'waiting' : 'live';
    return {frames, lastFrameAt, status};
  }

  function criticalFrameFresh(health, now, staleMs) {
    return Number.isFinite(staleMs) && staleMs > 0 && health.frames > 0 &&
        health.lastFrameAt != null && now >= health.lastFrameAt &&
        now - health.lastFrameAt < staleMs && health.status !== 'offline';
  }

  function mount(grid) {
    const entries = new Map();
    let disconnected = false;

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
      for (const entry of entries.values()) {
        if (entry.cleanup) entry.cleanup();
        entry.video.srcObject = null;
      }
      entries.clear();
      grid.replaceChildren();
      disconnected = false;
    }

    function configure(ids) {
      reset();
      for (const id of orderedCameraIds(ids)) ensure(id);
      reorder();
    }

    function presentedFrames(video) {
      const quality = video.getVideoPlaybackQuality();
      return Math.max(0, quality.totalVideoFrames - quality.droppedVideoFrames);
    }

    function refresh() {
      const now = performance.now();
      for (const entry of entries.values()) {
        entry.health = frameHealth(entry.health, {
          frames: presentedFrames(entry.video),
          now, startedAt: entry.startedAt,
          staleMs: entry.criticalForControl ? CRITICAL_DISPLAY_STALE_MS : STALE_MS,
          // Critical-view overlays follow actually presented frames. Muting
          // still withdraws control immediately in criticalHealth below.
          muted: !entry.criticalForControl && Boolean(entry.track && entry.track.muted), ended: Boolean(entry.track && entry.track.readyState === 'ended'), disconnected,
        });
        paint(entry, entry.health.status);
      }
    }

    function attach(id, track) {
      const entry = ensure(id);
      if (entry.cleanup) entry.cleanup();
      entry.track = track;
      entry.startedAt = performance.now();
      entry.video.srcObject = new MediaStream([track]);
      entry.health = {frames: presentedFrames(entry.video), lastFrameAt: null};
      entry.criticalRecoveryFrames = 0;
      entry.criticalFrameCounter = entry.health.frames;
      paint(entry, 'waiting');
      const update = () => refresh();
      for (const event of ['mute', 'unmute', 'ended']) track.addEventListener(event, update);
      entry.cleanup = () => {
        for (const event of ['mute', 'unmute', 'ended']) track.removeEventListener(event, update);
      };
      reorder();
    }

    function setConnectionState(state) {
      disconnected = ['disconnected', 'failed', 'closed'].includes(state);
      // frameHealth requires fresh frames after transport recovery.
      refresh();
    }

    function criticalHealth(ids, staleMs) {
      const criticalIds = new Set(ids);
      for (const [id, entry] of entries) entry.criticalForControl = criticalIds.has(id);
      refresh();
      const now = performance.now();
      const cameras = ids.map(id => {
        const entry = entries.get(id);
        if (!entry) return {camera_id: id, ready: false, reason: 'camera_missing'};
        const track = entry.track;
        const fresh = track && !track.muted && track.readyState !== 'ended' &&
            !disconnected && criticalFrameFresh(entry.health, now, staleMs);
        entry.criticalRecoveryFrames = fresh ? Math.min(3, (entry.criticalRecoveryFrames || 0) +
            Math.max(0, entry.health.frames - (entry.criticalFrameCounter || 0))) : 0;
        entry.criticalFrameCounter = entry.health.frames;
        const reason = disconnected ? 'peer_disconnected' : !track ? 'track_missing'
          : track.readyState === 'ended' ? 'track_ended' : track.muted ? 'track_muted'
          : entry.health.lastFrameAt == null ? 'no_presented_frame'
          : !fresh ? 'presented_frame_stale'
          : entry.criticalRecoveryFrames < 3 ? 'recovering_frames' : 'ready';
        return {camera_id: id, ready: fresh === true && entry.criticalRecoveryFrames >= 3,
          reason, frame_age_ms: entry.health.lastFrameAt == null ? null : Math.max(0, now - entry.health.lastFrameAt),
          presented_frames: entry.health.frames, recovery_frames: entry.criticalRecoveryFrames,
          track_state: track ? track.readyState : null, track_muted: Boolean(track && track.muted),
          video_ready_state: entry.video.readyState, video_paused: entry.video.paused,
          overlay_state: entry.health.status, stale_ms: staleMs};
      });
      const missing = cameras.filter(camera => !camera.ready).map(camera => camera.camera_id);
      return {ready: ids.length > 0 && missing.length === 0, missing, cameras};
    }

    return {configure, attach, reset, refresh, setConnectionState, criticalHealth};
  }

  return Object.freeze({CAMERAS, STALE_MS, CRITICAL_DISPLAY_STALE_MS, orderedCameraIds, cameraLabel, frameHealth, criticalFrameFresh, mount});
});
