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
    // Only an advancing presentation counter is a new sample. A delayed or
    // rolled-back observation must not renew freshness or clear a fault; a
    // genuine new track resets its baseline explicitly in attach().
    const resetWaitingTrack = frames === 0 && previous.lastFrameAt == null;
    frames = Number.isFinite(frames) && (frames >= previous.frames || resetWaitingTrack) ? frames : previous.frames;
    const advanced = frames > 0 && frames > previous.frames;
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
    // Only current entries hold per-source history. The aggregate survives
    // resets; its latest critical snapshot never accumulates former camera IDs.
    const sourceIdentities = {revision: 0, overflow: false};
    const revisions = {bad: 0, brake: 0, overflow: false};
    let revisionSnapshot = null;
    let disconnected = false;
    let connectionState = 'connected';

    function advanceRevision(record, key, amount = 1) {
      if (!Number.isSafeInteger(amount) || amount < 0 || amount > Number.MAX_SAFE_INTEGER - record[key]) {
        record[key] = Number.MAX_SAFE_INTEGER;
        record.overflow = true; // Never round/wrap an event back into a healthy window.
      } else record[key] += amount;
    }

    function nextSourceIdentity() {
      advanceRevision(sourceIdentities, 'revision');
      return sourceIdentities.revision;
    }

    function newCameraHistory() {
      const identity = nextSourceIdentity();
      return {identity, generation: identity, bad: 0, brake: 0, overflow: false};
    }

    function observeBadSamples(observation, count) {
      advanceRevision(observation.history, 'bad', count);
      const previous = observation.consecutiveAnomalies;
      observation.consecutiveAnomalies = Math.min(3, previous + count);
      if (previous < 3 && observation.consecutiveAnomalies === 3) advanceRevision(observation.history, 'brake');
    }

    function criticalRevisions(ids) {
      const next = ids.map(id => {
        const entry = entries.get(id);
        const history = entry ? entry.critical.history : null;
        return {id, identity: history ? history.identity : 0, generation: history ? history.generation : 0,
          bad: history ? history.bad : 0, brake: history ? history.brake : 0};
      });
      if (revisionSnapshot != null) {
        const identities = snapshot => JSON.stringify(snapshot.map(({id, generation}) => [id, generation]));
        if (identities(next) !== identities(revisionSnapshot)) {
          // Membership and track identity change revoke the prior observation
          // baseline, even if replacement frames arrive before the next poll.
          advanceRevision(revisions, 'bad');
          advanceRevision(revisions, 'brake');
        }
        const previous = new Map(revisionSnapshot.map(camera => [camera.id, camera]));
        for (const camera of next) {
          const before = previous.get(camera.id);
          if (!before) continue;
          // A new entry's counters belong to a new namespace. reset() folded
          // any outstanding old-source events before releasing that entry.
          advanceRevision(revisions, 'bad', camera.identity === before.identity ? camera.bad - before.bad : camera.bad);
          advanceRevision(revisions, 'brake', camera.identity === before.identity ? camera.brake - before.brake : camera.brake);
        }
      }
      revisionSnapshot = next;
      return {bad_sample_revision: revisions.bad, brake_sample_revision: revisions.brake,
        overflow: revisions.overflow || sourceIdentities.overflow || ids.some(id => {
          const entry = entries.get(id);
          return Boolean(entry && entry.critical.history.overflow);
        })};
    }

    function newCriticalTrack(frames, history) {
      return {frames, lastValidFrameAt: null, consecutiveAnomalies: 0,
        stateKey: null, sampleSeq: 0, awaitingValidFrame: true,
        recoveryFrames: 0, admitted: false, rawStateHealthy: false, history};
    }

    function observeCriticalTrack(entry, frames, now) {
      const observation = entry.critical;
      const track = entry.track;
      const videoUsable = entry.video.paused !== true &&
          (!Number.isFinite(entry.video.readyState) || entry.video.readyState >= 2);
      const stateHealthy = Boolean(track && track.readyState === 'live' && !track.muted &&
          !disconnected && videoUsable);
      // Changes are sampled from actual track/video/peer state, including events
      // between control polls. Re-reading the same state is not another sample.
      const stateKey = JSON.stringify([track ? track.readyState : 'missing',
        Boolean(track && track.muted), connectionState, videoUsable]);
      const stateChanged = observation.stateKey !== stateKey;
      if (stateChanged) {
        observation.stateKey = stateKey;
        observation.sampleSeq++;
        observation.awaitingValidFrame = true;
        observation.recoveryFrames = 0;
        if (!stateHealthy) observeBadSamples(observation, 1);
      }
      if (frames === 0 && observation.lastValidFrameAt == null) observation.frames = 0;
      const advanced = Number.isFinite(frames) ? Math.max(0, frames - observation.frames) : 0;
      if (advanced > 0) {
        observation.frames = frames;
        observation.sampleSeq += advanced;
        // A lifecycle event can arrive after main-thread delay, together with
        // frames from the previous state. Establish its baseline without
        // claiming those pictures are new normal/invalid data for this state.
        if (stateChanged) {
          observation.awaitingValidFrame = true;
        } else if (stateHealthy) {
          observation.lastValidFrameAt = now;
          observation.consecutiveAnomalies = 0;
          observation.awaitingValidFrame = false;
          observation.recoveryFrames = Math.min(3, observation.recoveryFrames + advanced);
          if (observation.recoveryFrames >= 3) observation.admitted = true;
        } else {
          observeBadSamples(observation, advanced);
        }
      }
      observation.rawStateHealthy = stateHealthy;
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
      const entry = {box, video, status, track: null, cleanup: null, startedAt: performance.now(), health: {frames: 0, lastFrameAt: null}, critical: newCriticalTrack(0, newCameraHistory())};
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
      if (revisionSnapshot != null) criticalRevisions(revisionSnapshot.map(camera => camera.id));
      for (const entry of entries.values()) {
        if (entry.cleanup) entry.cleanup();
        entry.video.srcObject = null;
      }
      entries.clear();
      grid.replaceChildren();
      disconnected = false;
      connectionState = 'connected';
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
        const frames = presentedFrames(entry.video);
        observeCriticalTrack(entry, frames, now);
        entry.health = frameHealth(entry.health, {
          frames,
          now, startedAt: entry.startedAt,
          staleMs: entry.criticalForControl ? CRITICAL_DISPLAY_STALE_MS : STALE_MS,
          // Critical overlays follow presented frames during short state faults.
          // Control qualification uses independent valid-frame age and samples.
          muted: !entry.criticalForControl && Boolean(entry.track && entry.track.muted),
          ended: !entry.criticalForControl && Boolean(entry.track && entry.track.readyState === 'ended'),
          disconnected: !entry.criticalForControl && disconnected,
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
      const history = entry.critical.history;
      history.generation = nextSourceIdentity();
      entry.critical = newCriticalTrack(entry.health.frames, history);
      observeCriticalTrack(entry, entry.health.frames, performance.now());
      paint(entry, 'waiting');
      const update = () => refresh();
      const trackEvents = ['mute', 'unmute', 'ended'];
      const videoEvents = ['pause', 'playing', 'waiting', 'canplay'];
      for (const event of trackEvents) track.addEventListener(event, update);
      for (const event of videoEvents) if (entry.video.addEventListener) entry.video.addEventListener(event, update);
      entry.cleanup = () => {
        for (const event of trackEvents) track.removeEventListener(event, update);
        for (const event of videoEvents) if (entry.video.removeEventListener) entry.video.removeEventListener(event, update);
      };
      reorder();
    }

    function setConnectionState(state) {
      connectionState = state;
      disconnected = ['disconnected', 'failed', 'closed'].includes(state);
      // State restoration is not normal video data; a new valid frame is needed.
      refresh();
    }

    function criticalHealth(ids, staleMs) {
      ids = orderedCameraIds(ids);
      const criticalIds = new Set(ids);
      for (const [id, entry] of entries) entry.criticalForControl = criticalIds.has(id);
      refresh();
      const now = performance.now();
      const cameras = ids.map(id => {
        const entry = entries.get(id);
        if (!entry) return {camera_id: id, ready: false, sample_healthy: false, reason: 'camera_missing'};
        const track = entry.track;
        const observed = entry.critical;
        const age = observed.lastValidFrameAt == null ? null : now - observed.lastValidFrameAt;
        const recentValid = Number.isFinite(staleMs) && staleMs > 0 && age != null &&
            age >= 0 && age < staleMs;
        const sampleHealthy = !observed.history.overflow && observed.rawStateHealthy && !observed.awaitingValidFrame && recentValid;
        // First/second state anomalies retain a previously admitted recent view.
        // They do not make the common recovery window healthy. No-new-data age
        // independently withdraws qualification even if only one event arrived.
        const qualified = !observed.history.overflow && observed.admitted && recentValid && observed.consecutiveAnomalies < 3;
        const reason = disconnected ? 'peer_disconnected' : !track ? 'track_missing'
          : track.readyState === 'ended' ? 'track_ended' : track.muted ? 'track_muted'
          : entry.video.paused === true ? 'video_paused'
          : Number.isFinite(entry.video.readyState) && entry.video.readyState < 2 ? 'video_not_ready'
          : observed.lastValidFrameAt == null ? 'no_presented_frame'
          : !recentValid ? 'presented_frame_stale'
          : observed.awaitingValidFrame ? 'awaiting_valid_frame'
          : !observed.admitted ? 'recovering_frames' : 'ready';
        return {camera_id: id, ready: qualified, sample_healthy: sampleHealthy,
          reason, frame_age_ms: age == null ? null : Math.max(0, age),
          presented_frames: observed.frames, recovery_frames: observed.recoveryFrames,
          consecutive_anomalies: observed.consecutiveAnomalies, sample_seq: observed.sampleSeq,
          bad_sample_revision: observed.history.bad, brake_sample_revision: observed.history.brake,
          track_state: track ? track.readyState : null, track_muted: Boolean(track && track.muted),
          video_ready_state: entry.video.readyState, video_paused: entry.video.paused,
          overlay_state: entry.health.status, stale_ms: staleMs};
      });
      const revision = criticalRevisions(ids);
      const missing = cameras.filter(camera => !camera.ready).map(camera => camera.camera_id);
      return {ready: !revision.overflow && ids.length > 0 && missing.length === 0,
        sample_healthy: !revision.overflow && ids.length > 0 && cameras.every(camera => camera.sample_healthy),
        bad_sample_revision: revision.bad_sample_revision, brake_sample_revision: revision.brake_sample_revision,
        missing, cameras};
    }

    function presentationSnapshot() {
      const now = performance.now();
      // Read the existing frame observations without qualifying or refreshing
      // tracks: taking metrics must not count as new video or safety recovery.
      return {sampled_at_monotonic_ms: now, timestamp_scope: 'browser_monotonic',
        frame_age_scope: 'since_last_observed_valid_presented_frame_not_capture_latency',
        cameras: orderedCameraIds([...entries.keys()]).map(id => {
          const entry = entries.get(id), observed = entry.critical;
          const age = observed.lastValidFrameAt == null ? null : now - observed.lastValidFrameAt;
          return {camera_id: id, critical_for_control: entry.criticalForControl === true,
            frame_age_ms: age != null && age >= 0 ? age : null,
            presented_frames: observed.frames, sample_seq: observed.sampleSeq,
            track_state: entry.track?.readyState ?? null,
            track_muted: entry.track?.muted === true,
            video_paused: entry.video.paused === true,
            video_ready_state: entry.video.readyState,
            overlay_state: entry.health.status};
        })};
    }

    return {configure, attach, reset, refresh, setConnectionState, criticalHealth, presentationSnapshot};
  }

  return Object.freeze({CAMERAS, STALE_MS, CRITICAL_DISPLAY_STALE_MS, orderedCameraIds, cameraLabel, frameHealth, criticalFrameFresh, mount});
});
