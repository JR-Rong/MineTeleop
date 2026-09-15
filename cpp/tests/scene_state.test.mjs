import {strict as assert} from 'assert';
const test=(name,run)=>{run();console.log(`PASS ${name}`);};
import {ViewState,validateManifest} from '../web/scene_state.mjs';
const packet=(speed,time=1000)=>({speed_mps:speed,sent_at_utc_ms:time});
test('valid measured speed selects rear view including reverse, then returns to top',()=>{
  const state=new ViewState({settleMs:50});
  state.receive(packet(-1),0,1000);assert.equal(state.step(0).mode,'top');assert.equal(state.step(51).mode,'rear');
  state.receive(packet(0),100,1000);state.step(100);assert.equal(state.step(151).mode,'top');
});
test('missing, invalid and stale telemetry never masquerades as a stopped vehicle',()=>{
  const state=new ViewState({settleMs:1});state.receive(packet(1),0,1000);state.step(0);state.step(2);
  assert.equal(state.step(501).fresh,false);assert.equal(state.step(501).mode,'rear');
  for(const value of [null,packet(null),packet(NaN),{speed_mps:0},packet(0,0)]){state.receive(value,1000,2000);const next=state.step(1002);assert.equal(next.fresh,false);assert.equal(next.mode,'rear');}
});
test('CAN valid/fresh flags take precedence over a generic speed field',()=>{
  const state=new ViewState();state.receive({...packet(0),can_feedback:{supported:true,speed_mps:2,speed_valid:true,feedback_fresh:false}},0,1000);assert.equal(state.step(0).fresh,false);
});
test('manual drag and grace period prevent automatic view changes',()=>{
  const state=new ViewState({resumeMs:100,settleMs:10});state.receive(packet(2),0,1000);state.beginManual();
  assert.equal(state.step(0).manual,true);state.endManual(20);assert.equal(state.step(119).mode,'top');
  state.step(120);assert.equal(state.step(131).mode,'rear');
});
test('zero-speed hysteresis and persistence suppress rapid toggling',()=>{
  const state=new ViewState({settleMs:50});state.receive(packet(0.09),0,1000);state.step(0);
  state.receive(packet(0.03),20,1000);state.step(20);state.receive(packet(0.09),40,1000);state.step(40);
  assert.equal(state.step(60).mode,'top');assert.equal(state.step(91).mode,'rear');
  state.receive(packet(0.06),100,1000);assert.equal(state.step(151).mode,'rear');
});
test('model transform and view settings are validated',()=>{
  assert.equal(validateManifest({file:'truck.obj'}).scale,1);
  for(const value of [{file:'truck.js'},{file:'truck.fbx',scale:0},{file:'truck.glb',rotation_deg:[0,NaN,0]},{file:'truck.glb',camera:{stopSpeed:1,moveSpeed:0}}])assert.throws(()=>validateManifest(value));
});
