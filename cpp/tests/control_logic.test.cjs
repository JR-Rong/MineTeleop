'use strict';

const assert = require('assert').strict;
const logic = require('../web/control_logic.js');

let passed = 0;
let failed = 0;
function test(name, body) {
  try {
    body();
    passed += 1;
    console.log(`[PASS] ${name}`);
  } catch (error) {
    failed += 1;
    console.error(`[FAIL] ${name}`);
    console.error(error && error.stack ? error.stack : error);
  }
}

const readyVcu = {
  supported: true,
  state: 'ready',
  ready: true,
  adapter_ready: true,
  speed_valid: true,
  speed_mps: 0,
};

test('fixed keyboard bindings expose two brakes and paired direction keys', () => {
  assert.deepEqual(logic.KEY_BINDINGS, {
    ArrowLeft: 'left', KeyA: 'left', ArrowRight: 'right', KeyD: 'right',
    ArrowUp: 'up', KeyW: 'up', ArrowDown: 'down', KeyS: 'down',
    Space: 'service_brake', KeyB: 'hard_brake',
  });
  assert.equal(Object.isFrozen(logic.KEY_BINDINGS), true);
});

test('physical key set keeps an action held until every mapped key is released', () => {
  const pressed = logic.createKeySet();
  const blocked = logic.createKeySet();
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowUp').changed, true);
  assert.equal(logic.pressKey(pressed, blocked, 'KeyW').changed, true);
  assert.equal(logic.deriveKeyState(pressed).up, true);
  assert.equal(logic.releaseKey(pressed, blocked, 'ArrowUp').changed, true);
  assert.equal(logic.deriveKeyState(pressed).up, true);
  assert.equal(logic.releaseKey(pressed, blocked, 'KeyW').changed, true);
  assert.equal(logic.deriveKeyState(pressed).up, false);
});

test('blocked keys require a physical release before a fresh press', () => {
  const pressed = logic.createKeySet(['ArrowLeft']);
  const blocked = logic.createKeySet();
  logic.blockAndClearKeys(pressed, blocked);
  assert.equal(pressed.size, 0);
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowLeft').reason, 'blocked_until_release');
  logic.releaseKey(pressed, blocked, 'ArrowLeft');
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowLeft').accepted, true);
});

test('gear selection latches on release and gates every new D/R selection on valid zero speed', () => {
  const forward = {up: true, down: false};
  const released = {up: false, down: false};
  const reverse = {up: false, down: true};
  assert.deepEqual(logic.deriveGearSelection('N', forward, readyVcu), {
    selectedGear: 'D', pendingGearRequest: null, changed: true,
  });
  assert.deepEqual(logic.deriveGearSelection('D', released, readyVcu), {
    selectedGear: 'D', pendingGearRequest: null, changed: false,
  });
  assert.deepEqual(logic.deriveGearSelection('N', forward, {...readyVcu, speed_valid: false}), {
    selectedGear: 'N', pendingGearRequest: 'D', changed: false,
  });
  assert.equal(logic.allowsGearChange('P', 'D', {...readyVcu, speed_valid: false}), false);
  assert.equal(logic.allowsGearChange('P', 'D', readyVcu), true);
  assert.deepEqual(logic.deriveGearSelection('D', reverse, {...readyVcu, speed_valid: false}), {
    selectedGear: 'D', pendingGearRequest: 'R', changed: false,
  });
  assert.equal(
      logic.deriveGearSelection('D', reverse, {...readyVcu, speed_mps: 0.1}).selectedGear,
      'R');
  assert.equal(
      logic.deriveGearSelection('D', reverse, {...readyVcu, speed_mps: -0.1}).selectedGear,
      'R');
  assert.equal(
      logic.deriveGearSelection('D', reverse, {...readyVcu, speed_mps: 0.1001}).pendingGearRequest,
      'R');
  assert.equal(
      logic.deriveGearSelection('D', reverse, {supported: false, state: 'unsupported'}).selectedGear,
      'R');
});

test('deriveControl applies brake priority, suppresses throttle, and preserves steering', () => {
  const limits = {maxThrottle: 0.05, serviceBrake: 0.3, hardBrake: 0.8, maxSteeringDeg: 3};
  const base = {gamepad: {steering: 0, throttle: 0, brake: 0}, selectedGear: 'D', limits,
    steeringFullScaleDeg: 30};
  const driving = logic.deriveControl({...base, keyState: {left: true, up: true}});
  assert.equal(driving.steering, -0.1);
  assert.equal(driving.throttle, 0.05);
  assert.equal(driving.brake, 0);

  const service = logic.deriveControl({
    ...base, keyState: {left: true, up: true, service_brake: true},
  });
  assert.equal(service.steering, -0.1);
  assert.equal(service.throttle, 0);
  assert.equal(service.brake, 0.3);

  const hard = logic.deriveControl({
    ...base, keyState: {right: true, up: true, service_brake: true, hard_brake: true},
  });
  assert.equal(hard.steering, 0.1);
  assert.equal(hard.throttle, 0);
  assert.equal(hard.brake, 0.8);

  const pedal = logic.deriveControl({
    ...base,
    keyState: {},
    gamepad: {steering: 0.5, throttle: 1, brake: 0.5},
  });
  assert.equal(pedal.steering, 0.05);
  assert.equal(pedal.throttle, 0);
  assert.equal(pedal.brake, 0.4);
});

test('deriveControl keeps mismatched or opposing direction requests at zero throttle', () => {
  const common = {
    gamepad: {steering: 0, throttle: 0, brake: 0},
    limits: {maxThrottle: 1, serviceBrake: 0.2, hardBrake: 1, maxSteeringDeg: 30},
    steeringFullScaleDeg: 30,
  };
  assert.equal(logic.deriveControl({...common, selectedGear: 'D', keyState: {down: true}}).throttle, 0);
  assert.equal(
      logic.deriveControl({...common, selectedGear: 'D', keyState: {up: true, down: true}}).throttle,
      0);
  assert.equal(
      logic.deriveControl({...common, selectedGear: 'N', keyState: {},
        gamepad: {steering: 0, throttle: 1, brake: 0}}).throttle,
      0);
});

test('a blocked Gamepad gear request requires physical pedal neutral before retry', () => {
  let gate = logic.reduceGamepadNeutralInterlock({
    requiresNeutral: false,
    authorityReady: true,
    throttle: 0.7,
    brake: 0,
    gearRequestPending: true,
  });
  assert.deepEqual(gate, {requiresNeutral: true, throttle: 0, brake: 0});

  gate = logic.reduceGamepadNeutralInterlock({
    requiresNeutral: gate.requiresNeutral,
    authorityReady: true,
    throttle: 0.7,
    brake: 0,
  });
  assert.deepEqual(gate, {requiresNeutral: true, throttle: 0, brake: 0});

  gate = logic.reduceGamepadNeutralInterlock({
    requiresNeutral: gate.requiresNeutral,
    authorityReady: true,
    throttle: 0,
    brake: 0,
  });
  assert.deepEqual(gate, {requiresNeutral: false, throttle: 0, brake: 0});

  gate = logic.reduceGamepadNeutralInterlock({
    requiresNeutral: gate.requiresNeutral,
    authorityReady: true,
    throttle: 0.7,
    brake: 0,
  });
  assert.deepEqual(gate, {requiresNeutral: false, throttle: 0.7, brake: 0});
});

test('status sequence rejects stale values and reports accepted gaps', () => {
  assert.deepEqual(logic.reduceStatusSequence(0, 1), {accepted: true, lastSequence: 1, gap: 0});
  assert.deepEqual(logic.reduceStatusSequence(1, 3), {accepted: true, lastSequence: 3, gap: 1});
  assert.deepEqual(logic.reduceStatusSequence(3, 2), {accepted: false, lastSequence: 3, gap: 0});
  assert.deepEqual(logic.reduceStatusSequence(3, 3), {accepted: false, lastSequence: 3, gap: 0});
  assert.deepEqual(logic.reduceStatusSequence(3, 'bad'), {accepted: false, lastSequence: 3, gap: 0});
});

test('ESTOP presentation distinguishes local and vehicle telemetry states', () => {
  assert.deepEqual(logic.deriveEstopPresentation(false, false), {
    kind: 'none', visible: false, severity: '', banner: '', alert: '',
  });

  const localPending = logic.deriveEstopPresentation(true, false);
  assert.equal(localPending.kind, 'local_pending');
  assert.equal(localPending.visible, true);
  assert.equal(localPending.severity, 'critical');
  assert.match(localPending.banner, /等待车端遥测确认/);

  const localConfirmed = logic.deriveEstopPresentation(true, true);
  assert.equal(localConfirmed.kind, 'local_confirmed');
  assert.equal(localConfirmed.severity, 'critical');
  assert.match(localConfirmed.banner, /已由车端遥测确认/);

  const vehicleOnly = logic.deriveEstopPresentation(false, true);
  assert.equal(vehicleOnly.kind, 'vehicle_only');
  assert.equal(vehicleOnly.visible, true);
  assert.equal(vehicleOnly.severity, 'critical');
  assert.match(vehicleOnly.banner, /不是由本页面请求/);

  assert.equal(logic.deriveEstopPresentation(false, false).visible, false);
});

test('peer and channel identity gates reject stale lifecycle callbacks', () => {
  const oldPeer = {};
  const currentPeer = {};
  const oldChannel = {};
  const currentChannel = {};

  assert.equal(logic.isCurrentPeer(currentPeer, oldPeer), false);
  assert.equal(logic.isCurrentPeer(currentPeer, currentPeer), true);
  assert.equal(
      logic.isCurrentControlChannel(currentPeer, oldPeer, currentChannel, oldChannel),
      false);
  assert.equal(
      logic.isCurrentControlChannel(currentPeer, currentPeer, currentChannel, oldChannel),
      false);
  assert.equal(
      logic.isCurrentControlChannel(currentPeer, currentPeer, currentChannel, currentChannel),
      true);

  let acceptedCallbacks = 0;
  const applyCallback = (callbackPeer, callbackChannel) => {
    if (!logic.isCurrentControlChannel(
        currentPeer, callbackPeer, currentChannel, callbackChannel)) return false;
    acceptedCallbacks += 1;
    return true;
  };
  assert.equal(applyCallback(oldPeer, oldChannel), false);
  assert.equal(applyCallback(currentPeer, oldChannel), false);
  assert.equal(acceptedCallbacks, 0);
  assert.equal(applyCallback(currentPeer, currentChannel), true);
  assert.equal(acceptedCallbacks, 1);
});

test('latest intent supersedes ordinary prepared commands but never an ESTOP', () => {
  const ordinary = logic.controlSnapshot({gear: 'D', steering: 0, throttle: 0.1, brake: 0, estop: false});
  assert.equal(logic.controlIntentSuperseded(ordinary, {...ordinary, throttle: 0}), true);
  assert.equal(logic.controlIntentSuperseded(ordinary, ordinary), false);
  const estop = logic.controlSnapshot({...ordinary, estop: true});
  assert.equal(logic.controlIntentSuperseded(estop, {...ordinary, throttle: 0}), false);
});

test('every prepared browser command has exactly one named terminal outcome', () => {
  assert.equal(logic.shouldLogControlOutcome('prepared'), false);
  assert.equal(logic.shouldLogControlOutcome('forwarded'), false);
  assert.equal(logic.shouldLogControlOutcome('superseded'), true);
  assert.equal(logic.shouldLogControlOutcome('post_prepare_link_changed'), true);
  assert.equal(logic.shouldLogControlOutcome('post_prepare_vcu_not_ready'), true);
  assert.throws(() => logic.shouldLogControlOutcome('delivered'), /unknown control outcome/);
  let metrics = logic.createControlOutcomeMetrics();
  let sequence = 10;
  for (const outcome of logic.CONTROL_OUTCOMES) {
    metrics = logic.reduceControlOutcome(metrics, 'prepared', sequence);
    assert.equal(logic.controlOutcomesBalanced(metrics), false);
    metrics = logic.reduceControlOutcome(metrics, outcome, sequence);
    assert.equal(logic.controlOutcomesBalanced(metrics), true);
    sequence += 1;
  }
  assert.deepEqual(metrics, {
    prepared: 4,
    forwarded: 1,
    superseded: 1,
    post_prepare_link_changed: 1,
    post_prepare_vcu_not_ready: 1,
    last_prepared_seq: 13,
    last_forwarded_seq: 10,
  });
  assert.throws(
      () => logic.reduceControlOutcome(metrics, 'forwarded', 13),
      /does not match/);
});

test('VCU reducer retains only authorized convergence waits and resets terminal or adapter loss', () => {
  assert.equal(logic.adapterReady({state: 'unsupported'}), true);
  assert.equal(logic.adapterReady({state: 'fault'}), null);
  assert.equal(logic.adapterReady({state: 'ready'}, false), false);
  assert.equal(logic.drivingReady(readyVcu), true);
  assert.equal(logic.drivingReady({...readyVcu, adapter_ready: false}), false);

  assert.deepEqual(
      logic.transitionVcuState(true, {...readyVcu, state: 'wait_gear', ready: false}),
      {everReady: true, resetInput: false, retainedWait: true});
  assert.deepEqual(
      logic.transitionVcuState(true, {...readyVcu, state: 'wait_actuator_modes', ready: false}),
      {everReady: true, resetInput: false, retainedWait: true});
  assert.equal(logic.transitionVcuState(true, {...readyVcu, state: 'fault', ready: false}).resetInput, true);
  assert.equal(logic.transitionVcuState(true, {...readyVcu, adapter_ready: false}).resetInput, true);
  assert.equal(logic.transitionVcuState(false, readyVcu).everReady, true);
});

if (failed > 0) {
  console.error(`${failed} of ${passed + failed} control logic tests failed`);
  process.exitCode = 1;
} else {
  console.log(`${passed} control logic tests passed`);
}
