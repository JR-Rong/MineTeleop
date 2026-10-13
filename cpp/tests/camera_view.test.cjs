'use strict';
const assert = require('assert').strict;
const {orderedCameraIds, cameraLabel, frameHealth, criticalFrameFresh} = require('../web/camera_view.js');
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
test('critical-video freshness uses confirmed timing before the visual three-second overlay', () => {
  const live = sample(empty, 100, 1);
  assert(criticalFrameFresh(live, 599, 500));
  assert(!criticalFrameFresh(live, 600, 500));
  assert.equal(sample(live, 600, 1, {staleMs:500}).status, 'stale');
  assert(!criticalFrameFresh(empty, 100, 500));
  assert(!criticalFrameFresh(live, 99, 500));
  assert(!criticalFrameFresh(sample(live, 200, 2, {muted:true}), 200, 500));
  assert(criticalFrameFresh(sample(live, 700, 2), 700, 500));
  assert(criticalFrameFresh(live, 999, 1000));
});
test('critical overlay waits one second independently of braking threshold and resets membership', () => {
  const fs=require('fs'),vm=require('vm');let now=0;
  class Element {
    constructor(tag){this.tag=tag;this.children=[];this.dataset={};this.frames=0;this.hidden=false;}
    append(...items){this.children.push(...items)}
    replaceChildren(...items){this.children=items}
    insertBefore(item,before){this.children=this.children.filter(x=>x!==item);const i=this.children.indexOf(before);this.children.splice(i<0?this.children.length:i,0,item)}
    setAttribute(){}
    getVideoPlaybackQuality(){return {totalVideoFrames:this.frames,droppedVideoFrames:0}}
  }
  const context=vm.createContext({document:{createElement:tag=>new Element(tag)},performance:{now:()=>now},MediaStream:class {}});
  vm.runInContext(fs.readFileSync(require.resolve('../web/camera_view.js'),'utf8'),context);
  const api=context.MineTeleopCameraView,grid=new Element('grid'),view=api.mount(grid);
  view.configure(['front','aux']);
  const track={muted:false,readyState:'live',addEventListener(){},removeEventListener(){}};
  view.attach('front',track);view.attach('aux',{...track});
  const front=grid.children.find(x=>x.dataset.cameraId==='front'),aux=grid.children.find(x=>x.dataset.cameraId==='aux');
  for(now=100;now<=166;now+=33){front.children[1].frames++;aux.children[1].frames++;view.criticalHealth(['front'],500)}
  now=666;assert.equal(view.criticalHealth(['front'],500).ready,false);assert.equal(front.dataset.state,'live');
  now=1165;view.refresh();assert.equal(front.dataset.state,'live');
  now=1166;view.refresh();assert.equal(front.dataset.state,'stale');assert.equal(aux.dataset.state,'live');
  view.criticalHealth([],500);assert.equal(front.dataset.state,'live');
  now=3166;view.refresh();assert.equal(front.dataset.state,'stale');assert.equal(aux.dataset.state,'stale');
  now=4000;view.configure(['front']);view.attach('front',track);
  const second=grid.children[0];
  for(now=4100;now<=4166;now+=33){second.children[1].frames++;view.criticalHealth(['front'],2000)}
  now=5266;assert.equal(view.criticalHealth(['front'],2000).ready,true);assert.equal(second.dataset.state,'stale');
  now=6000;second.children[1].frames++;view.criticalHealth(['front'],500);track.muted=true;
  now=6100;assert.equal(view.criticalHealth(['front'],500).ready,false);assert.equal(second.dataset.state,'live');
  now=7000;view.refresh();assert.equal(second.dataset.state,'stale');
});
process.exitCode = failed ? 1 : 0;
