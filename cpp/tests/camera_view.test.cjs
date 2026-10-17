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
test('out-of-order presentation counters cannot refresh a frozen view', () => {
  let state = sample(empty, 100, 10);
  state = sample(state, 599, 9);
  assert.equal(state.frames, 10);
  assert.equal(state.lastFrameAt, 100);
  state = sample(state, 600, 10);
  assert.equal(criticalFrameFresh(state, 600, 500), false);
  assert.equal(sample(state, 650, 11).lastFrameAt, 650);
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
  aux.children[1].frames=0; // A non-critical frozen view must not withdraw the front view.
  assert.equal(view.criticalHealth(['front'],500).ready,true);
  now=666;assert.equal(view.criticalHealth(['front'],500).ready,false);assert.equal(front.dataset.state,'live');
  now=1165;view.refresh();assert.equal(front.dataset.state,'live');
  now=1166;view.refresh();assert.equal(front.dataset.state,'stale');assert.equal(aux.dataset.state,'live');
  assert.equal(view.criticalHealth([],500).ready,false); // Unknown/empty metadata is not authority.
  assert.equal(front.dataset.state,'live');
  now=3166;view.refresh();assert.equal(front.dataset.state,'stale');assert.equal(aux.dataset.state,'stale');
  now=4000;view.configure(['front']);view.attach('front',track);
  const second=grid.children[0];
  for(now=4100;now<=4166;now+=33){second.children[1].frames++;view.criticalHealth(['front'],2000)}
  now=5266;assert.equal(view.criticalHealth(['front'],2000).ready,true);assert.equal(second.dataset.state,'stale');
  now=6000;second.children[1].frames++;view.criticalHealth(['front'],500);track.muted=true;
  now=6100;const firstMute=view.criticalHealth(['front'],500);
  assert.equal(firstMute.ready,true);assert.equal(firstMute.sample_healthy,false);
  assert.equal(firstMute.cameras[0].consecutive_anomalies,1);assert.equal(second.dataset.state,'live');
  now=6500;assert.equal(view.criticalHealth(['front'],500).ready,false); // Same original no-new-frame boundary.
  now=7000;view.refresh();assert.equal(second.dataset.state,'stale');
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'track_muted');
  track.muted=false;track.readyState='ended';
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'track_ended');
  view.setConnectionState('disconnected');
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'peer_disconnected');
  view.setConnectionState('connected');
  assert.equal(view.criticalHealth(['missing'],500).cameras[0].reason,'camera_missing');
  view.configure(['front']);
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'track_missing');
  track.readyState='live';view.attach('front',track);
  const video=grid.children[0].children[1];
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'no_presented_frame');
  video.frames++;
  assert.equal(view.criticalHealth(['front'],500).cameras[0].reason,'recovering_frames');
  for(let i=0;i<5;i++)assert.equal(view.criticalHealth(['front'],500).cameras[0].recovery_frames,1);
  video.frames+=2;assert.equal(view.criticalHealth(['front'],500).ready,true);
  now+=500;const stale=view.criticalHealth(['front'],500).cameras[0];
  assert.equal(stale.reason,'presented_frame_stale');assert.equal(stale.frame_age_ms,500);
  now++;video.frames+=3;assert.equal(view.criticalHealth(['front'],500).ready,true);
  // Replacing a track must gather three new frames even if the previous track was ready.
  view.attach('front',track);video.frames++;
  assert.equal(view.criticalHealth(['front'],500).ready,false);

});
function cameraFixture(ids=['front']) {
  const fs=require('fs'),vm=require('vm');let now=0;
  class Element {
    constructor(tag){this.tag=tag;this.children=[];this.dataset={};this.frames=0;this.hidden=false;this.paused=false;this.readyState=4;}
    append(...items){this.children.push(...items)}
    replaceChildren(...items){this.children=items}
    insertBefore(item,before){this.children=this.children.filter(x=>x!==item);const i=this.children.indexOf(before);this.children.splice(i<0?this.children.length:i,0,item)}
    setAttribute(){}
    getVideoPlaybackQuality(){return {totalVideoFrames:this.frames,droppedVideoFrames:0}}
  }
  function newTrack(){
    const listeners=new Map();
    return {muted:false,readyState:'live',
      addEventListener(name,fn){if(!listeners.has(name))listeners.set(name,new Set());listeners.get(name).add(fn)},
      removeEventListener(name,fn){const group=listeners.get(name);if(group)group.delete(fn)},
      emit(name){for(const fn of listeners.get(name)||[])fn()}};
  }
  const context=vm.createContext({document:{createElement:tag=>new Element(tag)},performance:{now:()=>now},MediaStream:class {}});
  vm.runInContext(fs.readFileSync(require.resolve('../web/camera_view.js'),'utf8'),context);
  const grid=new Element('grid'),view=context.MineTeleopCameraView.mount(grid),tracks={};
  view.configure(ids);
  for(const id of ids){tracks[id]=newTrack();view.attach(id,tracks[id]);}
  const video=id=>grid.children.find(x=>x.dataset.cameraId===id).children[1];
  const health=(critical=ids,ms=500)=>view.criticalHealth(critical,ms);
  function bootstrap(){
    for(const time of [100,133,166]){now=time;for(const id of ids)video(id).frames++;health();}
    assert.equal(health().ready,true);assert.equal(health().sample_healthy,true);
  }
  return {view,tracks,video,health,bootstrap,newTrack,time(value){now=value}};
}

test('three real mute transitions tolerate two samples and repeated polls never clear or accumulate', () => {
  const f=cameraFixture();f.bootstrap();const track=f.tracks.front;
  f.time(200);track.muted=true;track.emit('mute');
  let health=f.health();assert.equal(health.ready,true);assert.equal(health.sample_healthy,false);
  assert.equal(health.cameras[0].consecutive_anomalies,1);const sampleSeq=health.cameras[0].sample_seq;
  for(let i=0;i<100;i++){track.emit('mute');health=f.health();assert.equal(health.cameras[0].consecutive_anomalies,1);assert.equal(health.cameras[0].sample_seq,sampleSeq)}
  // Normal state transitions alone are not a valid normal presented sample.
  f.time(210);track.muted=false;track.emit('unmute');assert.equal(f.health().sample_healthy,false);assert.equal(f.health().cameras[0].consecutive_anomalies,1);
  f.time(220);track.muted=true;track.emit('mute');assert.equal(f.health().ready,true);assert.equal(f.health().cameras[0].consecutive_anomalies,2);
  f.time(230);track.muted=false;track.emit('unmute');
  f.time(240);track.muted=true;track.emit('mute');health=f.health();assert.equal(health.ready,false);assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].consecutive_anomalies,3);
  f.time(250);track.muted=false;track.emit('unmute');assert.equal(f.health().ready,false);assert.equal(f.health().cameras[0].consecutive_anomalies,3);
  f.time(260);f.video('front').frames++;health=f.health();assert.equal(health.sample_healthy,true);assert.equal(health.cameras[0].consecutive_anomalies,0);
  // Qualification is evidence only; the native 300ms/release-repress gate still owns driving permission.
  assert.equal(health.ready,true);
});

test('short mute and ended state cannot retain qualification beyond the original valid-frame timeout', () => {
  for(const kind of ['mute','ended']){
    const f=cameraFixture();f.bootstrap();const track=f.tracks.front;
    f.time(200);if(kind==='mute')track.muted=true;else track.readyState='ended';track.emit(kind);
    f.time(665);assert.equal(f.health().ready,true);assert.equal(f.health().sample_healthy,false);assert.equal(f.health().cameras[0].consecutive_anomalies,1);
    f.time(666);assert.equal(f.health().ready,false);assert.equal(f.health().cameras[0].frame_age_ms,500);assert.equal(f.health().cameras[0].consecutive_anomalies,1);
    f.time(1165);assert.equal(f.health(['front'],1000).ready,true);
    f.time(1166);assert.equal(f.health(['front'],1000).ready,false);
  }
});

test('new invalid presented pictures count separately while duplicate and rolled-back counters cannot clear faults', () => {
  const f=cameraFixture();f.bootstrap();const track=f.tracks.front;
  f.time(200);track.muted=true;track.emit('mute');
  f.time(220);f.video('front').frames++;let health=f.health();assert.equal(health.ready,true);assert.equal(health.cameras[0].consecutive_anomalies,2);assert.equal(health.cameras[0].frame_age_ms,54);
  const seq=health.cameras[0].sample_seq;
  for(let i=0;i<20;i++){health=f.health();assert.equal(health.cameras[0].consecutive_anomalies,2);assert.equal(health.cameras[0].sample_seq,seq)}
  f.time(230);f.video('front').frames++;health=f.health();assert.equal(health.ready,false);assert.equal(health.cameras[0].consecutive_anomalies,3);assert.equal(health.cameras[0].frame_age_ms,64);
  f.time(240);track.muted=false;track.emit('unmute');assert.equal(f.health().sample_healthy,false);
  f.video('front').frames=4;assert.equal(f.health().cameras[0].consecutive_anomalies,3);assert.equal(f.health().sample_healthy,false);
  f.time(260);f.video('front').frames=6;health=f.health();assert.equal(health.cameras[0].consecutive_anomalies,0);assert.equal(health.sample_healthy,true);
  f.time(1000);f.video('front').frames=5;health=f.health();assert.equal(health.ready,false);assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].frame_age_ms,740);
});

test('actual peer transitions count once each and restoration alone cannot clear a third anomaly', () => {
  const f=cameraFixture();f.bootstrap();
  for(const [index,state] of ['disconnected','failed','closed'].entries()){
    f.time(200+index*20);f.view.setConnectionState(state);let health=f.health();
    assert.equal(health.cameras[0].consecutive_anomalies,index+1);assert.equal(health.ready,index<2);assert.equal(health.sample_healthy,false);
    const seq=health.cameras[0].sample_seq;
    for(let i=0;i<5;i++){f.view.setConnectionState(state);health=f.health();assert.equal(health.cameras[0].sample_seq,seq);assert.equal(health.cameras[0].consecutive_anomalies,index+1)}
  }
  f.time(270);f.view.setConnectionState('connected');assert.equal(f.health().ready,false);assert.equal(f.health().sample_healthy,false);
  f.time(280);f.video('front').frames++;assert.equal(f.health().sample_healthy,true);assert.equal(f.health().cameras[0].consecutive_anomalies,0);
});

test('healthy optional video cannot clear a critical-camera fault and unknown configuration remains blocked', () => {
  const f=cameraFixture(['front','aux']);f.bootstrap();
  for(let i=0;i<3;i++){
    f.time(200+i*20);f.tracks.front.muted=true;f.tracks.front.emit('mute');f.video('aux').frames++;f.health(['front']);
    if(i<2){f.tracks.front.muted=false;f.tracks.front.emit('unmute');}
  }
  let health=f.health(['front']);assert.equal(health.ready,false);assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].consecutive_anomalies,3);
  f.time(300);f.video('aux').frames++;assert.equal(f.health(['aux']).ready,true);health=f.health(['front','aux']);assert.equal(health.ready,false);assert.equal(health.sample_healthy,false);
  assert.equal(f.health([]).ready,false);assert.equal(f.health([]).sample_healthy,false);
  assert.equal(f.health(['missing']).ready,false);assert.equal(f.health(['missing']).sample_healthy,false);
});

test('replacement track needs new admission frames and cannot borrow the old valid-frame age', () => {
  const f=cameraFixture();f.bootstrap();const replacement=f.newTrack();
  f.time(200);f.view.attach('front',replacement);assert.equal(f.health().ready,false);assert.equal(f.health().sample_healthy,false);
  f.video('front').frames=0;assert.equal(f.health().cameras[0].recovery_frames,0);
  for(let i=1;i<=3;i++){f.time(210+i*20);f.video('front').frames=i;const health=f.health();assert.equal(health.ready,i===3);assert.equal(health.cameras[0].recovery_frames,i)}
  f.view.configure(['front']);f.time(10000);assert.equal(f.health().ready,false);assert.equal(f.health().sample_healthy,false);assert.equal(f.health().cameras[0].reason,'track_missing');
});

test('paused video and valid state without a new presented frame cannot clear faults', () => {
  const f=cameraFixture();f.bootstrap();f.time(200);f.video('front').paused=true;
  let health=f.health();assert.equal(health.ready,true);assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].consecutive_anomalies,1);
  f.video('front').frames+=2;health=f.health();assert.equal(health.ready,false);assert.equal(health.cameras[0].consecutive_anomalies,3);
  f.time(220);f.video('front').paused=false;assert.equal(f.health().ready,false);assert.equal(f.health().sample_healthy,false);
  f.time(240);f.video('front').frames++;health=f.health();assert.equal(health.sample_healthy,true);assert.equal(health.cameras[0].consecutive_anomalies,0);
});

test('delayed lifecycle observation cannot misclassify accumulated pictures as three anomalies or normal recovery', () => {
  const f=cameraFixture();f.bootstrap();const track=f.tracks.front;
  f.time(200);f.video('front').frames+=10;track.muted=true;track.emit('mute');
  let health=f.health();assert.equal(health.ready,true);assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].consecutive_anomalies,1);
  assert.equal(health.cameras[0].frame_age_ms,34); // No arrival-time relabelling of old-state pictures.
  f.time(220);f.video('front').frames+=10;track.muted=false;track.emit('unmute');
  health=f.health();assert.equal(health.sample_healthy,false);assert.equal(health.cameras[0].consecutive_anomalies,1);assert.equal(health.cameras[0].frame_age_ms,54);
  f.time(240);f.video('front').frames++;health=f.health();assert.equal(health.sample_healthy,true);assert.equal(health.cameras[0].consecutive_anomalies,0);assert.equal(health.cameras[0].frame_age_ms,0);
});

test('a complete raw bad-to-good episode between control reads preserves its revision without braking', () => {
  const f=cameraFixture();f.bootstrap();const before=f.health(),track=f.tracks.front;
  // Actual listeners/timer update the shipped view while the control reader is idle.
  f.time(200);track.muted=true;track.emit('mute');
  f.time(210);track.muted=false;track.emit('unmute');
  f.time(220);f.video('front').frames++;f.view.refresh();
  const after=f.health();
  assert.equal(after.ready,true);assert.equal(after.sample_healthy,true);
  assert.equal(after.cameras[0].consecutive_anomalies,0);
  assert.equal(after.bad_sample_revision,before.bad_sample_revision+1);
  assert.equal(after.brake_sample_revision,before.brake_sample_revision);
  assert.equal(after.cameras[0].bad_sample_revision,before.cameras[0].bad_sample_revision+1);
  assert.equal(Number.isSafeInteger(after.bad_sample_revision),true);
  assert.equal(Number.isSafeInteger(after.brake_sample_revision),true);
  for(let i=0;i<20;i++){
    const repeated=f.health();assert.equal(repeated.bad_sample_revision,after.bad_sample_revision);
    assert.equal(repeated.brake_sample_revision,after.brake_sample_revision);
  }
});

test('a third-anomaly brake followed by normal video between reads remains an event and does not repeat above three', () => {
  const f=cameraFixture();f.bootstrap();const before=f.health(),track=f.tracks.front;
  for(let i=0;i<3;i++){
    f.time(200+i*20);track.muted=true;track.emit('mute');
    f.time(210+i*20);track.muted=false;track.emit('unmute');
  }
  f.time(270);f.video('front').frames++;f.view.refresh();
  let after=f.health();assert.equal(after.ready,true);assert.equal(after.sample_healthy,true);
  assert.equal(after.cameras[0].consecutive_anomalies,0);
  assert.equal(after.bad_sample_revision,before.bad_sample_revision+3);
  assert.equal(after.brake_sample_revision,before.brake_sample_revision+1);
  const firstEpisode=after;
  f.time(280);track.muted=true;track.emit('mute');
  f.time(290);f.video('front').frames+=4;f.view.refresh();
  f.time(300);track.muted=false;track.emit('unmute');
  f.time(310);f.video('front').frames++;f.view.refresh();
  after=f.health();assert.equal(after.sample_healthy,true);
  assert.equal(after.bad_sample_revision,firstEpisode.bad_sample_revision+5);
  assert.equal(after.brake_sample_revision,firstEpisode.brake_sample_revision+1);
});

test('duplicate and rolled-back presented observations cannot change retained fault revisions', () => {
  const f=cameraFixture();f.bootstrap();const track=f.tracks.front;
  f.time(200);track.muted=true;track.emit('mute');const bad=f.health();
  for(let i=0;i<100;i++){
    track.emit('mute');f.video('front').frames=2;const repeated=f.health();
    assert.equal(repeated.bad_sample_revision,bad.bad_sample_revision);
    assert.equal(repeated.brake_sample_revision,bad.brake_sample_revision);
    assert.equal(repeated.cameras[0].consecutive_anomalies,1);
  }
  f.time(210);track.muted=false;track.emit('unmute');f.video('front').frames=3;f.view.refresh();
  assert.equal(f.health().sample_healthy,false);
  assert.equal(f.health().bad_sample_revision,bad.bad_sample_revision);
  f.time(220);f.video('front').frames=4;const normal=f.health();
  assert.equal(normal.sample_healthy,true);assert.equal(normal.bad_sample_revision,bad.bad_sample_revision);
});

test('critical revision membership is canonical and optional-camera bad events cannot reset its window', () => {
  const f=cameraFixture(['front','aux']);f.bootstrap();
  const both=f.health(['front','aux']);const reordered=f.health(['aux','front','front']);
  assert.equal(reordered.bad_sample_revision,both.bad_sample_revision);
  assert.equal(reordered.brake_sample_revision,both.brake_sample_revision);
  const front=f.health(['front']);
  for(let i=0;i<3;i++){
    f.time(200+i*20);f.tracks.aux.muted=true;f.tracks.aux.emit('mute');
    f.time(210+i*20);f.tracks.aux.muted=false;f.tracks.aux.emit('unmute');
  }
  f.time(270);f.video('aux').frames++;f.view.refresh();
  const unchanged=f.health(['front']);assert.equal(unchanged.sample_healthy,true);
  assert.equal(unchanged.bad_sample_revision,front.bad_sample_revision);
  assert.equal(unchanged.brake_sample_revision,front.brake_sample_revision);
  const changed=f.health(['front','aux']);assert.equal(changed.sample_healthy,true);
  assert.equal(changed.bad_sample_revision,front.bad_sample_revision+1);
  assert.equal(changed.brake_sample_revision,front.brake_sample_revision+1);
});

test('replacement and reset completed between control reads cannot reuse the previous healthy revision baseline', () => {
  const f=cameraFixture();f.bootstrap();const before=f.health();
  f.time(200);f.view.attach('front',f.newTrack());f.video('front').frames=0;f.view.refresh();
  for(let i=1;i<=3;i++){f.time(210+i*20);f.video('front').frames=i;f.view.refresh();}
  const replaced=f.health();assert.equal(replaced.ready,true);assert.equal(replaced.sample_healthy,true);
  assert.equal(replaced.bad_sample_revision,before.bad_sample_revision+1);
  assert.equal(replaced.brake_sample_revision,before.brake_sample_revision+1);
  f.time(300);f.view.configure(['front']);f.view.attach('front',f.newTrack());
  for(let i=1;i<=3;i++){f.time(310+i*20);f.video('front').frames=i;f.view.refresh();}
  const reset=f.health();assert.equal(reset.ready,true);assert.equal(reset.sample_healthy,true);
  assert.equal(reset.bad_sample_revision,replaced.bad_sample_revision+1);
  assert.equal(reset.brake_sample_revision,replaced.brake_sample_revision+1);
  assert.equal(reset.cameras[0].bad_sample_revision,replaced.cameras[0].bad_sample_revision);
});

test('reset folds unread critical faults before releasing old source history and camera IDs do not accumulate', () => {
  const f=cameraFixture();f.bootstrap();const before=f.health(),oldTrack=f.tracks.front;
  f.time(200);oldTrack.muted=true;oldTrack.emit('mute');
  f.time(210);oldTrack.muted=false;oldTrack.emit('unmute');
  f.time(220);f.video('front').frames++;f.view.refresh();
  // No control read occurred before configure discarded the original entry.
  f.time(230);f.view.configure(['front']);f.view.attach('front',f.newTrack());
  for(let i=1;i<=3;i++){f.time(240+i*20);f.video('front').frames=i;f.view.refresh();}
  let after=f.health();assert.equal(after.sample_healthy,true);
  assert.equal(after.bad_sample_revision,before.bad_sample_revision+2); // unread fault + new source baseline
  assert.equal(after.brake_sample_revision,before.brake_sample_revision+1);
  assert.equal(after.cameras[0].bad_sample_revision,0); // Former entry/history was released.
  for(let i=0;i<50;i++){
    const id='vehicle-'+i,previous=after;
    f.view.configure([id]);f.view.attach(id,f.newTrack());
    for(let frame=1;frame<=3;frame++){f.time(400+i*100+frame*20);f.video(id).frames=frame;f.view.refresh();}
    after=f.health([id]);assert.equal(after.ready,true);assert.equal(after.cameras.length,1);
    assert.equal(after.cameras[0].bad_sample_revision,0);
    assert.equal(after.bad_sample_revision,previous.bad_sample_revision+1);
    assert.equal(after.brake_sample_revision,previous.brake_sample_revision+1);
  }
  oldTrack.muted=true;oldTrack.emit('mute');const detached=f.health(['vehicle-49']);
  assert.equal(detached.bad_sample_revision,after.bad_sample_revision);
  assert.equal(detached.brake_sample_revision,after.brake_sample_revision);
});

process.exitCode = failed ? 1 : 0;
