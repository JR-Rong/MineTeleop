'use strict';
const assert = require('assert').strict;
const {orderedCameraIds, cameraLabel, frameHealth} = require('../web/camera_view.js');
let failed = 0;
function test(name, body) {
  try { body(); console.log('[PASS] ' + name); }
  catch (error) { failed++; console.error('[FAIL] ' + name, error); }
}
const empty = {frames: 0, lastFrameAt: null};
const sample = (previous, now, frames, extra = {}) => frameHealth(previous, {now, frames, startedAt: 0, ...extra});
test('eight named views sort independently of arrival order', () => {
  assert.deepEqual(orderedCameraIds(['fish_right', 'drive_right', 'fish_front', 'drive_left', 'fish_left', 'drive_rear', 'fish_rear', 'drive_front']),
    ['drive_front', 'drive_rear', 'drive_left', 'drive_right', 'fish_front', 'fish_rear', 'fish_left', 'fish_right']);
});
test('sparse lists do not invent cameras and legacy IDs remain deterministic', () => {
  assert.deepEqual(orderedCameraIds(['z-custom', 'fish_right', 'a-custom', 'drive_front', 'drive_front', '', null]),
    ['drive_front', 'fish_right', 'a-custom', 'z-custom']);
  assert.equal(cameraLabel('fish_left'), '左鱼眼');
  assert.equal(cameraLabel('drive_rear'), '后视驾驶');
  assert.equal(cameraLabel('<legacy>'), '<legacy>');
  assert.deepEqual(orderedCameraIds([]), []);
});
test('a declared stream without a frame becomes stale after three seconds', () => {
  assert.equal(sample(empty, 0, 0).status, 'waiting');
  assert.equal(sample(empty, 2999, 0).status, 'waiting');
  assert.equal(sample(empty, 3000, 0).status, 'stale');
});
test('frozen frames expire and new frames recover in the same state', () => {
  let state = sample(empty, 100, 1);
  assert.equal(state.status, 'live');
  state = sample(state, 3100, 1);
  assert.equal(state.status, 'stale');
  assert.equal(sample(state, 3200, 2).status, 'live');
});
test('muted or ended tracks cannot appear live even with buffered frames', () => {
  const live = sample(empty, 100, 1);
  const muted = sample(live, 200, 2, {muted: true});
  assert.equal(muted.status, 'stale');
  assert.notEqual(sample(muted, 250, 2).status, 'live');
  assert.equal(sample(muted, 250, 3).status, 'live');
  assert.equal(sample(live, 200, 2, {ended: true}).status, 'offline');
});
test('transport recovery requires a fresh frame', () => {
  const disconnected = sample(sample(empty, 100, 1), 200, 2, {disconnected: true});
  assert.equal(disconnected.status, 'offline');
  assert.notEqual(sample(disconnected, 250, 2).status, 'live');
  assert.equal(sample(disconnected, 300, 3).status, 'live');
});
test('reset frame counters wait for the first frame of a new track', () => {
  const reset = sample({frames: 100, lastFrameAt: null}, 0, 0);
  assert.equal(reset.status, 'waiting');
  assert.equal(sample(reset, 100, 1).status, 'live');
});
process.exitCode = failed ? 1 : 0;
