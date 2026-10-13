'use strict';

const assert = require('assert').strict;
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const logic = require('../web/control_logic.js');
const controlPageSource = fs.readFileSync(path.join(__dirname, '../src/server.cpp'), 'utf8');

let passed = 0;
let failed = 0;
const pendingTests = [];
function test(name, body) {
  function pass() {
    passed += 1;
    console.log(`[PASS] ${name}`);
  }
  function fail(error) {
    failed += 1;
    console.error(`[FAIL] ${name}`);
    console.error(error && error.stack ? error.stack : error);
  }
  try {
    const result = body();
    if (result && typeof result.then === 'function') {
      pendingTests.push(Promise.resolve(result).then(pass, fail));
    } else {
      pass();
    }
  } catch (error) {
    fail(error);
  }
}

test('shipped browser logs identify the loaded page without changing diagnostic payloads', () => {
  const requests = [];
  const context = vm.createContext({
    consoleConfig: {console_source_sha256: 'built-page-source', diagnostics_revision: 'traction-diagnostics-v1'},
    uiInstanceId: 'page-instance', Date,
    console: {info: () => {}},
    fetch: (url, options) => {requests.push(JSON.parse(options.body)); return Promise.resolve({});},
  });
  const handler = controlPageSource.match(/^function clientLog\(event,details=\{\}\).*$/m);
  assert(handler, 'shipped browser event handler was not found');
  vm.runInContext(handler[0], context);
  context.clientLog('control_intent_response_timeout', {
    intent_seq: 62, input_retained: true, page_source_sha256: 'forged-or-stale',
  });
  const details = requests[0].details;
  assert.equal(details.intent_seq, 62);
  assert.equal(details.input_retained, true);
  assert.equal(details.page_source_sha256, 'built-page-source');
  assert.equal(details.diagnostics_revision, 'traction-diagnostics-v1');
  assert.equal(details.ui_instance_id, 'page-instance');
});

const readyVcu = {
  supported: true,
  state: 'ready',
  ready: true,
  adapter_ready: true,
  speed_valid: true,
  speed_mps: 0,
  gear_change_stationary_confirmed: true,
};

// Execute the shipped page's request handler, with deterministic local HTTP
// responses and timers. A synthetic reducer would miss its input-clearing bug.
function intentPageFixture(codes = ['ArrowUp', 'ArrowLeft']) {
  const pressed = new Set(codes);
  const blocked = new Set();
  const logs = [];
  const timers = new Map();
  const requests = [];
  let timerId = 0;
  let elapsed = 1000;
  const context = {
    controlLogic: logic,
    peer: {connectionState: 'connected'},
    controlChannel: {readyState: 'open'},
    controlAuthorityLost: false,
    gearRejectionInhibited: false,
    controlProfileState: {acknowledged: true},
    vcuHandshake: readyVcu,
    vehicleTelemetry: null,
    driverCriticalVideoHealth: () => ({ready: true, missing: []}),
    driverTimeSyncReady: () => true,
    pendingGearTransition: null,
    estopLatched: false,
    activeControlPreparePreemptedByEstop: false,
    nativeControlSessionId: 'test-session',
    nativeControlSessionGeneration: 1,
    lastNativeIntentSnapshot: 'held-input',
    consoleConfig: {intent_lease_ms: 200},
    performance: {now: () => elapsed},
    webrtcLabel: {}, statusPanel: {}, lastKeyboardEvent: {},
    vcuStateKeepsHeldInput: () => false,
    vcuDrivingReady: () => true,
    currentControl: () => {
      const keys = logic.deriveKeyState(pressed);
      return {gear: 'D', throttle: keys.up ? 0.5 : 0,
        steering: keys.left ? -1 : keys.right ? 1 : 0,
        brake: 0, estop: context.estopLatched};
    },
    nativeIntentEnvelope: outgoing => Object.assign({
      session_id: context.nativeControlSessionId,
      session_generation: context.nativeControlSessionGeneration,
      intent_seq: 62,
    }, outgoing),
    controlInputDiagnostic: () => ({pressed_codes: Array.from(pressed), blocked_codes: Array.from(blocked)}),
    clearControlInput: (resetGear, reason) => {
      logic.blockAndClearKeys(pressed, blocked);
      logs.push({event: 'cleared', reason});
    },
    resetControlAuthorityInput: () => { context.controlAuthorityLost = true; logic.blockAndClearKeys(pressed, blocked); },
    resetControlProfileSession: () => { context.controlProfileState.acknowledged = false; },
    clientLog: (event, details) => logs.push({event, details}),
    setTimeout: callback => { const id = ++timerId; timers.set(id, callback); return id; },
    clearTimeout: id => timers.delete(id),
    AbortController: class {
      constructor() { this.signal = {}; }
      abort() { if (this.signal.onAbort) this.signal.onAbort(); }
    },
    post: (url, intent, signal) => new Promise((resolve, reject) => {
      signal.onAbort = () => reject(Object.assign(new Error('delayed local response'), {name: 'AbortError'}));
      requests.push({url, intent, resolve, reject});
    }),
  };
  vm.createContext(context);
  const start = controlPageSource.indexOf('async function writeControlIntent(');
  const end = controlPageSource.indexOf('function reportControlQueueError(', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(controlPageSource.slice(start, end), context);
  return {context, pressed, blocked, logs, requests,
    timeout: () => { elapsed += 150; Array.from(timers.values()).forEach(callback => callback()); }};
}

test('delayed local intent response keeps all four held directions and can refresh again', async () => {
  for (const code of ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight']) {
    const fixture = intentPageFixture([code]);
    const first = fixture.context.writeControlIntent({}, false);
    fixture.timeout();
    assert.equal((await first).reason, 'control_intent_update_timeout');
    assert.deepEqual(Array.from(fixture.pressed), [code]);
    assert.equal(fixture.blocked.size, 0);
    assert.equal(fixture.logs[0].details.input_retained, true);
    assert.equal(fixture.logs[0].details.request_elapsed_ms, 150);
    const next = fixture.context.writeControlIntent({}, false);
    fixture.requests[1].resolve({accepted: true, duplicate: true, intent_seq: 62});
    assert.equal((await next).sent, true);
    assert.equal(fixture.pressed.has(code), true);
  }
});

test('response timeout cannot erase a newer key or resurrect a released key', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  const first = fixture.context.writeControlIntent({}, false);
  logic.releaseKey(fixture.pressed, fixture.blocked, 'ArrowUp');
  logic.pressKey(fixture.pressed, fixture.blocked, 'ArrowLeft');
  fixture.timeout();
  await first;
  assert.deepEqual(Array.from(fixture.pressed), ['ArrowLeft']);
  const next = fixture.context.writeControlIntent({}, false);
  assert.equal(fixture.requests[1].intent.throttle, 0);
  assert.equal(fixture.requests[1].intent.steering, -1);
  fixture.requests[1].resolve({accepted: true, duplicate: false, intent_seq: 63});
  await next;
});

test('real native lease rejection still blocks held keys until physical release', async () => {
  const fixture = intentPageFixture();
  const request = fixture.context.writeControlIntent({}, false);
  fixture.requests[0].resolve({accepted: false, reason: 'fresh_neutral_required', requires_fresh_input: true});
  assert.equal((await request).sent, false);
  assert.equal(fixture.pressed.size, 0);
  assert.equal(logic.pressKey(fixture.pressed, fixture.blocked, 'ArrowUp').accepted, false);
  assert.equal(fixture.logs[0].reason, 'native_intent_rearm_required');
  logic.releaseKey(fixture.pressed, fixture.blocked, 'ArrowUp');
  assert.equal(logic.pressKey(fixture.pressed, fixture.blocked, 'ArrowUp').accepted, true);
});

test('prior session timeout preserves current keys and ESTOP preemption still neutralizes', async () => {
  const fixture = intentPageFixture();
  const stale = fixture.context.writeControlIntent({}, false);
  fixture.context.nativeControlSessionGeneration = 2;
  fixture.timeout();
  assert.equal((await stale).reason, 'stale_control_intent_response');
  assert.equal(fixture.pressed.size, 2);
  assert.equal(fixture.logs.length, 0);
  const urgent = fixture.context.writeControlIntent({}, false);
  fixture.context.activeControlPreparePreemptedByEstop = true;
  fixture.context.estopLatched = true;
  fixture.timeout();
  assert.equal((await urgent).reason, 'control_intent_preempted_by_estop');
  assert.equal(fixture.context.estopLatched, true);
  assert.equal(fixture.pressed.size, 0);
  assert.equal(fixture.logs[0].reason, 'estop_preemption');
  assert.equal(fixture.logs.some(entry => entry.event === 'control_intent_update_timeout'), false);
});

test('missing vehicle profile still neutralizes input and forbids driving', async () => {
  const fixture = intentPageFixture();
  fixture.context.controlProfileState.acknowledged = false;
  const request = fixture.context.writeControlIntent({}, true);
  assert.equal(fixture.requests[0].intent.throttle, 0);
  assert.equal(fixture.requests[0].intent.steering, 0);
  assert.equal(fixture.logs[0].reason, 'control_profile_not_acknowledged');
  fixture.requests[0].resolve({accepted: true, duplicate: true});
  assert.equal((await request).blocked_reason, 'control_profile_not_acknowledged');
});

test('duplicate profile telemetry preserves operator guidance but revocation still resets input', () => {
  const profile = controlProfile();
  const logs = [];
  const resets = [];
  const context = {
    controlLogic: logic,
    controlProfileState: {requestedProfile: profile, pendingRequestSeq: 7},
    pendingControlProfileEnvelope: {},
    statusPanel: {},
    clientLog: event => logs.push(event),
    resetControlAuthorityInput: reason => resets.push(reason),
    renderControlLimits: () => {}, renderMonitoring: () => {},
  };
  vm.createContext(context);
  const start = controlPageSource.indexOf('function applyControlProfileStatus(');
  const end = controlPageSource.indexOf('function controlProfileParkingReady(', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(controlPageSource.slice(start, end), context);
  const status = {accepted: true, active: true, last_request_seq: 7,
    applied_revision: 7, effective_profile: profile, reason: 'accepted'};
  assert.equal(context.applyControlProfileStatus(status), true);
  context.statusPanel.textContent = '请释放后重新按下';
  assert.equal(context.applyControlProfileStatus(status), true);
  assert.equal(context.statusPanel.textContent, '请释放后重新按下');
  assert.deepEqual(logs, ['session_control_profile_accepted']);
  context.applyControlProfileStatus({...status, accepted: false, active: false,
    reason: 'session_profile_cleared'});
  assert.equal(context.controlProfileState.acknowledged, false);
  assert.deepEqual(resets, ['session_control_profile_invalidated']);
});

test('telemetry freshness separates local receipt age from synchronized source age', () => {
  const telemetry = {sent_at_utc_ms: 100000};
  // A browser clock 40 seconds ahead must not make fresh telemetry look delayed.
  assert.deepEqual(logic.telemetryFreshness(telemetry, 1000, 1100, 140100 - 40000), {
    received_age_ms: 100, source_age_ms: 100, fresh: true,
  });
  assert.deepEqual(logic.telemetryFreshness(telemetry, 1000, 41100, 140100), {
    received_age_ms: 40100, source_age_ms: 40100, fresh: false,
  });
  assert.equal(logic.telemetryFreshness(telemetry, 1000, 1001, 140100).fresh, false,
      'a newly received but old source record became fresh');
  assert.equal(logic.telemetryFreshness(telemetry, 1000, 1001, null).fresh, false);
  assert.equal(logic.telemetryFreshness(null, null, 41100, 140100).received_age_ms, null);
});

test('ICE diagnostics follow the transport selected pair instead of another successful check', () => {
  const active = {id: 'active', type: 'candidate-pair', state: 'succeeded', nominated: false};
  const unused = {id: 'unused', type: 'candidate-pair', state: 'succeeded', nominated: true};
  const transport = {id: 'dtls', type: 'transport', selectedCandidatePairId: 'active'};
  const report = new Map([['unused', unused], ['active', active], ['dtls', transport]]);
  assert.deepEqual(logic.selectedIcePair(report), {pair: active, transport, method: 'transport'});
  assert.equal(logic.selectedIcePair(new Map([['active', active]])).pair, null);
  assert.equal(logic.selectedIcePair(new Map([['unused', unused]])).method, 'nominated_fallback');
});

test('browser native intent envelope is bound to the connected session generation', () => {
  assert.equal(
      controlPageSource.includes(
          "return{session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_instance_id:uiInstanceId,intent_seq:nativeIntentSeq,...normalized}"),
      true);
  assert.equal(
      controlPageSource.includes(
          "nativeControlSessionId=String(session.session_id||'');nativeControlSessionGeneration=Number(session.control_session_generation)"),
      true);
  assert.equal(
      controlPageSource.includes(
          "nativeControlSessionId='';nativeControlSessionGeneration=0;lastNativeIntentSnapshot=''"),
      true);
});

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

test('stationary evidence requires three fresh samples spanning at least 200 ms', () => {
  let evidence = logic.createGearChangeStationaryEvidence();
  evidence = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: true, speed_mps: 0.1}, 10, 1000);
  assert.equal(evidence.confirmed, false);
  evidence = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: true, speed_mps: -0.1}, 11, 1100);
  assert.equal(evidence.confirmed, false);
  evidence = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: true, speed_mps: 0}, 12, 1200);
  assert.deepEqual(evidence, {
    firstObservedAtMs: 1000,
    lastObservedAtMs: 1200,
    lastStatusSeq: 12,
    sampleCount: 3,
    confirmed: true,
  });

  const duplicate = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: true, speed_mps: 0}, 12, 1300);
  assert.strictEqual(duplicate, evidence);
  const moving = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: true, speed_mps: 0.1001}, 13, 1300);
  assert.equal(moving.confirmed, false);
  assert.equal(moving.sampleCount, 0);
  const stale = logic.updateGearChangeStationaryEvidence(
      evidence, {speed_valid: false, speed_mps: 0}, 13, 1300);
  assert.equal(stale.confirmed, false);
  assert.equal(stale.sampleCount, 0);
});

test('gear selection latches on release and gates every new D/R selection on stable zero speed', () => {
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
  assert.equal(
      logic.allowsGearChange('D', 'R', {...readyVcu, gear_change_stationary_confirmed: false}),
      false);
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

test('gear rejection matches only a forwarded command from the active transition', () => {
  let transition = logic.createGearTransition('D', 'R', 40, 7);
  assert.deepEqual(transition, {
    generation: 7,
    fromGear: 'D',
    toGear: 'R',
    statusFloor: 40,
    forwardedSeqs: [],
  });
  assert.strictEqual(
      logic.recordForwardedGearCommand(transition, 6, 100, 'R'), transition);
  assert.strictEqual(
      logic.recordForwardedGearCommand(transition, 7, 100, 'D'), transition);
  transition = logic.recordForwardedGearCommand(transition, 7, 101, 'R');
  transition = logic.recordForwardedGearCommand(transition, 7, 102, 'R');
  assert.deepEqual(transition.forwardedSeqs, [101, 102]);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    intent_seq: 101,
    command_seq: 999,
    control_status_seq: 41,
  }, 'R'), true);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    intent_seq: 103,
    command_seq: 101,
    control_status_seq: 41,
  }, 'R'), false);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    command_seq: 101,
    control_status_seq: 41,
  }, 'R'), true);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    intent_seq: 0,
    command_seq: 102,
    control_status_seq: 41,
  }, 'R'), true);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    command_seq: 101,
    control_status_seq: 40,
  }, 'R'), false);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    command_seq: 101,
    control_status_seq: 41,
  }, 'D'), false);

  assert.deepEqual(logic.reduceGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    intent_seq: 101,
    control_status_seq: 41,
  }, 'R'), {
    matched: true,
    selectedGear: 'D',
    pendingGearRequest: 'R',
    pendingGearTransition: null,
    inhibitOrdinaryControl: false,
    sendRollback: true,
  });
  assert.deepEqual(logic.reduceGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    command_seq: 999,
    control_status_seq: 41,
  }, 'R'), {
    matched: false,
    selectedGear: 'R',
    pendingGearRequest: null,
    pendingGearTransition: null,
    inhibitOrdinaryControl: true,
    sendRollback: false,
  });
});

test('only fresh authoritative telemetry confirms an active gear transition', () => {
  const unforwarded = logic.createGearTransition('D', 'R', 50, 8);
  const telemetry = {
    event: 'vehicle_telemetry',
    control_status_seq: 51,
    gear: 'N',
    can_feedback: {supported: true, feedback_fresh: true, gear_valid: true, gear: 2},
  };
  assert.equal(logic.telemetryConfirmsGearTransition(unforwarded, telemetry), false);
  const transition = logic.recordForwardedGearCommand(unforwarded, 8, 201, 'R');
  assert.equal(logic.telemetryConfirmsGearTransition(transition, telemetry), true);
  assert.equal(logic.telemetryConfirmsGearTransition(
      transition, {...telemetry, control_status_seq: 50}), false);
  assert.equal(logic.telemetryConfirmsGearTransition(
      transition, {...telemetry, can_feedback: {...telemetry.can_feedback, feedback_fresh: false}}),
      false);
  assert.equal(logic.telemetryConfirmsGearTransition(
      transition, {...telemetry, can_feedback: {...telemetry.can_feedback, gear_valid: false}}),
      false);
  assert.equal(logic.telemetryConfirmsGearTransition(
      transition, {...telemetry, can_feedback: {...telemetry.can_feedback, gear: 3}}), false);
  assert.equal(logic.telemetryConfirmsGearTransition(transition, {
    event: 'vehicle_telemetry',
    control_status_seq: 52,
    gear: 'R',
    can_feedback: {supported: false},
  }), true);
  for (const canFeedback of [undefined, {}, {supported: null}, {supported: 'false'}]) {
    assert.equal(logic.telemetryConfirmsGearTransition(transition, {
      event: 'vehicle_telemetry',
      control_status_seq: 52,
      gear: 'R',
      can_feedback: canFeedback,
    }), false);
  }
});

test('deriveControl applies brake priority, suppresses throttle, and preserves steering', () => {
  const limits = {maxThrottle: 0.05, maxBrakePressureBar: 100,
    serviceBrakePressureBar: 30, hardBrakePressureBar: 80, maxSteeringDeg: 3};
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
  assert.equal(pedal.brake, 0.5);

  const fullPedal = logic.deriveControl({
    ...base,
    keyState: {},
    gamepad: {steering: 0, throttle: 0, brake: 1},
  });
  assert.equal(fullPedal.brake, 1);
  const fullPedalWithHardKey = logic.deriveControl({
    ...base,
    keyState: {hard_brake: true},
    gamepad: {steering: 0, throttle: 0, brake: 1},
  });
  assert.equal(fullPedalWithHardKey.brake, 1);
});

test('deriveControl keeps mismatched or opposing direction requests at zero throttle', () => {
  const common = {
    gamepad: {steering: 0, throttle: 0, brake: 0},
    limits: {maxThrottle: 1, maxBrakePressureBar: 100,
      serviceBrakePressureBar: 20, hardBrakePressureBar: 100, maxSteeringDeg: 30},
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

test('control rejection presentation exposes only stable issue-code guidance', () => {
  const rearm = logic.deriveControlCommandRejection('control_input_rearm_required');
  assert.equal(rearm.action, 'rearm_input');
  assert.equal(rearm.clearInput, true);
  assert.equal(rearm.severity, 'warn');
  const pressed = logic.createKeySet(['ArrowUp', 'ArrowLeft']);
  const blocked = logic.createKeySet();
  logic.blockAndClearKeys(pressed, blocked);
  assert.equal(logic.deriveKeyState(pressed).up, false);
  assert.equal(logic.deriveKeyState(pressed).left, false);
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowUp').accepted, false);
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowLeft').accepted, false);
  logic.releaseKey(pressed, blocked, 'ArrowUp');
  logic.releaseKey(pressed, blocked, 'ArrowLeft');
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowUp').accepted, true);
  assert.equal(logic.pressKey(pressed, blocked, 'ArrowLeft').accepted, true);

  const gearRejected = logic.deriveControlCommandRejection(
      'vcu_drive_gear_change_moving_or_stale');
  assert.deepEqual(gearRejected, {
    issueCode: 'vcu_drive_gear_change_moving_or_stale',
    action: 'rollback_gear_change',
    clearInput: true,
    severity: 'warn',
    text: '换挡被车端拒绝：已撤销牵引并保持拒绝前挡位；请停车、恢复新鲜反馈，释放后再重新选择方向。',
  });

  const unknown = logic.deriveControlCommandRejection(
      'untrusted exception text / secret sentinel');
  assert.equal(unknown.issueCode, 'vcu_control_apply_rejected');
  assert.equal(unknown.action, 'reset_neutral');
  assert.equal(unknown.clearInput, true);
  assert.equal(unknown.severity, 'critical');
  assert(!unknown.text.includes('secret sentinel'));
});

function controlProfile(overrides = {}) {
  return {
    profile_version: 7,
    parking_idle_timeout_ms: 500,
    consecutive_late_frame_limit: 3,
    critical_camera_stale_brake_ms: 500,
    control_recovery_timeout_ms: 5000,
    target_speed_kph: 12,
    max_motor_torque_nm: 300,
    max_brake_pressure_bar: 90,
    service_brake_pressure_bar: 30,
    hard_brake_pressure_bar: 80,
    max_steering_angle_deg: 6,
    speed_pid_kp: 2,
    speed_pid_ki: 0.3,
    speed_pid_kd: 0.2,
    speed_pid_derivative_filter_tau_ms: 60,
    speed_pid_max_dt_ms: 80,
    motor_torque_rise_rate_nm_per_s: 120,
    ...overrides,
  };
}

function readOnlyControlSafety(overrides = {}) {
  return {
    control_rate_hz: 20,
    max_command_gap_ms: 200,
    degraded_timeout_ms: 300,
    control_timeout_ms: 800,
    deceleration_profile: [
      {after_ms: 0, brake: 0.3},
      {after_ms: 500, brake: 0.6},
      {after_ms: 1500, brake: 1},
    ],
    speed_feedback_timeout_ms: 200,
    hard_overspeed_margin_kph: 3.6,
    require_can_feedback_before_control: true,
    require_local_estop_reset: true,
    require_time_sync: true,
    max_time_sync_uncertainty_ms: 25,
    time_sync_interval_ms: 30000,
    time_sync_samples: 7,
    commissioning_mode: 'bench',
    ...overrides,
  };
}

function vehicleHardLimits(overrides = {}) {
  return {
    max_speed_kph: 40,
    max_throttle: 0.1,
    full_scale_motor_torque_nm: 165,
    max_brake_pressure_bar: 50,
    max_steering_angle_deg: 8,
    default_critical_camera_stale_brake_ms: 500,
    default_control_recovery_timeout_ms: 5000,
    default_speed_pid_kp: 1.5,
    default_speed_pid_ki: 0.2,
    default_speed_pid_kd: 0.1,
    default_speed_pid_derivative_filter_tau_ms: 50,
    default_speed_pid_max_dt_ms: 100,
    default_motor_torque_rise_rate_nm_per_s: 150,
    motor_torque_rise_rate_limits_nm_per_s: {min: 0, max: 400},
    speed_pid_limits: {
      kp: {min: 0, max: 10},
      ki: {min: 0, max: 10},
      kd: {min: 0, max: 10},
      derivative_filter_tau_ms: {min: 0, max: 500},
      max_dt_ms: {min: 20, max: 200},
    },
    speed_feedback_timeout_ms: 200,
    hard_overspeed_margin_kph: 3.6,
    speed_feedback_timeout_ms_read_only: true,
    hard_overspeed_margin_kph_read_only: true,
    read_only_control_safety: readOnlyControlSafety(),
    ...overrides,
  };
}

test('session control profile V7 validation and hard-limit merge preserve ordering', () => {
  const requested = controlProfile();
  const hard = vehicleHardLimits();
  assert.deepEqual(logic.mergeControlProfileWithHardLimits(requested, hard), {
    profile_version: 7,
    parking_idle_timeout_ms: 500,
    consecutive_late_frame_limit: 3,
    critical_camera_stale_brake_ms: 500,
    control_recovery_timeout_ms: 5000,
    target_speed_kph: 4,
    max_motor_torque_nm: 165,
    max_brake_pressure_bar: 50,
    service_brake_pressure_bar: 30,
    hard_brake_pressure_bar: 50,
    max_steering_angle_deg: 6,
    speed_pid_kp: 2,
    speed_pid_ki: 0.3,
    speed_pid_kd: 0.2,
    speed_pid_derivative_filter_tau_ms: 60,
    speed_pid_max_dt_ms: 80,
    motor_torque_rise_rate_nm_per_s: 120,
  });
  assert.equal(logic.mergeControlProfileWithHardLimits(
      {...requested, motor_torque_rise_rate_nm_per_s: 999}, hard)
      .motor_torque_rise_rate_nm_per_s, 400);
  assert.equal(logic.controlProfileThrottleLimit(
      {...requested, target_speed_kph: 2}, hard), 0.05);
  assert.equal(logic.controlProfileThrottleLimit(
      {...requested, target_speed_kph: 2}, {...hard, max_speed_kph: 0}), 0);
  assert.equal(logic.normalizeVehicleHardLimits(
      {...hard, max_target_speed_kph: 20}).max_target_speed_kph, 4);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, service_brake_pressure_bar: 80.1}),
      /service_brake_pressure_bar <= hard_brake_pressure_bar/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, max_motor_torque_nm: 640.1}),
      /max_motor_torque_nm/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({...hard, full_scale_motor_torque_nm: 640.1}),
      /full_scale_motor_torque_nm/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, target_speed_kph: '2'}),
      /target_speed_kph/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, speed_pid_kp: 0}),
      /speed_pid_kp must be greater than 0/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, speed_pid_max_dt_ms: 80.5}),
      /speed_pid_max_dt_ms must be an integer/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, profile_version: 1}),
      /profile_version must be 7/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, profile_version: 2}),
      /profile_version must be 7/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, unexpected_field: 1}),
      /exactly the V7 fields/);
  assert.throws(
      () => logic.normalizeControlProfile(
          {...requested, motor_torque_rise_rate_nm_per_s: -1}),
      /motor_torque_rise_rate_nm_per_s/);
  assert.throws(
      () => logic.normalizeControlProfile(
          {...requested, motor_torque_rise_rate_nm_per_s: 32000.1}),
      /motor_torque_rise_rate_nm_per_s/);
  const v2Profile = controlProfile();
  delete v2Profile.motor_torque_rise_rate_nm_per_s;
  assert.throws(
      () => logic.normalizeControlProfile(v2Profile),
      /exactly the V7 fields/);
});

test('PID defaults come only from complete vehicle limits and accept kp hard min zero', () => {
  const hard = vehicleHardLimits({
    default_critical_camera_stale_brake_ms: 500,
    default_control_recovery_timeout_ms: 5000,
    default_speed_pid_kp: 3.25,
    default_speed_pid_ki: 0.75,
    default_speed_pid_kd: 0.5,
    default_speed_pid_derivative_filter_tau_ms: 75,
    default_speed_pid_max_dt_ms: 120,
  });
  const profile = logic.controlProfileFromVehicleDefaults({
    target_speed_kph: 2,
    max_motor_torque_nm: 100,
    max_brake_pressure_bar: 50,
    service_brake_pressure_bar: 20,
    hard_brake_pressure_bar: 50,
    max_steering_angle_deg: 3,
  }, hard);
  assert.equal(profile.target_speed_kph, hard.max_speed_kph * hard.max_throttle / 2);
  assert.equal(profile.max_motor_torque_nm, hard.full_scale_motor_torque_nm / 2);
  assert.equal(profile.max_brake_pressure_bar, hard.max_brake_pressure_bar / 2);
  assert.equal(profile.max_steering_angle_deg, hard.max_steering_angle_deg);
  assert.equal(profile.parking_idle_timeout_ms, 500);
  assert.equal(profile.consecutive_late_frame_limit, 3);
  for (const limit of [1,2,3,4,5])
    assert.equal(logic.normalizeControlProfile({...profile, consecutive_late_frame_limit:limit}).consecutive_late_frame_limit, limit);
  for (const limit of [0,6,1.5,NaN,'3'])
    assert.throws(() => logic.normalizeControlProfile({...profile, consecutive_late_frame_limit:limit}), /consecutive_late_frame_limit/);
  for (const timeout of [0, 1000])
    assert.equal(logic.normalizeControlProfile({...profile, parking_idle_timeout_ms: timeout}).parking_idle_timeout_ms, timeout);
  for (const timeout of [-1, 1001, 0.5, NaN])
    assert.throws(() => logic.normalizeControlProfile({...profile, parking_idle_timeout_ms: timeout}), /parking_idle_timeout_ms/);
  assert.equal(profile.speed_pid_kp, 3.25);
  assert.equal(profile.speed_pid_ki, 0.75);
  assert.equal(profile.speed_pid_kd, 0.5);
  assert.equal(profile.speed_pid_derivative_filter_tau_ms, 75);
  assert.equal(profile.speed_pid_max_dt_ms, 120);
  assert.equal(profile.motor_torque_rise_rate_nm_per_s, 150);
  assert.equal(logic.normalizeVehicleHardLimits(hard).speed_pid_limits.kp.min, 0);

  const missingDefault = vehicleHardLimits();
  delete missingDefault.default_speed_pid_kd;
  assert.throws(() => logic.normalizeVehicleHardLimits(missingDefault), /default_speed_pid_kd/);
  const missingRiseRate = vehicleHardLimits();
  delete missingRiseRate.default_motor_torque_rise_rate_nm_per_s;
  assert.throws(
      () => logic.normalizeVehicleHardLimits(missingRiseRate),
      /default_motor_torque_rise_rate_nm_per_s/);
  const missingRiseRateLimits = vehicleHardLimits();
  delete missingRiseRateLimits.motor_torque_rise_rate_limits_nm_per_s;
  assert.throws(
      () => logic.normalizeVehicleHardLimits(missingRiseRateLimits),
      /motor_torque_rise_rate_limits_nm_per_s/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        motor_torque_rise_rate_limits_nm_per_s: {min: 500, max: 400},
      }),
      /must not exceed max/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({...vehicleHardLimits(), speed_pid_limits: null}),
      /speed_pid_limits/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(), default_speed_pid_kp: 0,
      }),
      /default_speed_pid_kp must be greater than 0/);
});

test('vehicle fixed safety limits are exact, read-only, consistent, and preserved', () => {
  const normalized = logic.normalizeVehicleHardLimits(vehicleHardLimits());
  assert.equal(normalized.speed_feedback_timeout_ms, 200);
  assert.equal(normalized.hard_overspeed_margin_kph, 3.6);
  assert.deepEqual(normalized.read_only_control_safety, readOnlyControlSafety());

  const missingSafety = vehicleHardLimits();
  delete missingSafety.read_only_control_safety;
  assert.throws(
      () => logic.normalizeVehicleHardLimits(missingSafety),
      /read_only_control_safety/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(), speed_feedback_timeout_ms_read_only: false,
      }),
      /explicitly read-only/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(), speed_feedback_timeout_ms: 201,
      }),
      /must match/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({unexpected_field: 1}),
      }),
      /exactly the fixed fields/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({
          deceleration_profile: [
            {after_ms: 0, brake: 0.6},
            {after_ms: 500, brake: 0.3},
            {after_ms: 1500, brake: 1},
          ],
        }),
      }),
      /brake must not decrease/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({
          require_time_sync: 'true',
        }),
      }),
      /require_time_sync must be boolean/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({control_rate_hz: 19}),
      }),
      /control_rate_hz/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({
          deceleration_profile: [
            {after_ms: 0, brake: 0.3},
            {after_ms: 2147483648, brake: 1},
          ],
        }),
      }),
      /after_ms/);
  assert.throws(
      () => logic.normalizeVehicleHardLimits({
        ...vehicleHardLimits(),
        read_only_control_safety: readOnlyControlSafety({
          speed_feedback_timeout_ms: 500,
          control_timeout_ms: 400,
        }),
        speed_feedback_timeout_ms: 500,
      }),
      /timeout ordering is invalid/);
});

test('motion rearm retains held keyboard and analog brakes, with authority and expiry gates intact', () => {
  const pressed=new Set(['ArrowUp','ArrowLeft','Space','KeyB']),blocked=new Set();
  logic.blockAndClearKeys(pressed,blocked,true);
  assert.deepEqual([...pressed],['Space','KeyB']);
  assert.deepEqual([...blocked],['ArrowUp','ArrowLeft']);
  assert.equal(logic.pressKey(pressed,blocked,'ArrowUp').changed,false);
  logic.releaseKey(pressed,blocked,'ArrowUp');
  assert.equal(logic.pressKey(pressed,blocked,'ArrowUp').changed,true);
  const request={requiresNeutral:true,authorityReady:true,throttle:1,brake:0.7,steering:0.8,preserveBrakeDuringRearm:true};
  assert.deepEqual(logic.reduceGamepadNeutralInterlock(request),{requiresNeutral:true,throttle:0,brake:0.7});
  assert.equal(logic.reduceGamepadNeutralInterlock({...request,throttle:0,brake:0}).requiresNeutral,true);
  assert.equal(logic.reduceGamepadNeutralInterlock({...request,throttle:0,brake:0,steering:0}).requiresNeutral,false);
  assert.equal(logic.reduceGamepadNeutralInterlock({...request,authorityReady:false}).brake,0);
  assert.equal(logic.reduceGamepadNeutralInterlock({...request,preserveBrakeDuringRearm:false}).brake,0);
  logic.blockAndClearKeys(pressed,blocked);
  assert.equal(pressed.size,0);
});

test('browser and C++ share the current profile version and require the complete V7 fields', () => {
  const header=fs.readFileSync(path.join(__dirname,'../include/mine_teleop/core.hpp'),'utf8');
  const version=Number(header.match(/kSessionControlProfileVersion = (\d+);/)[1]);
  assert.equal(logic.normalizeControlProfile(controlProfile()).profile_version,version);
  assert.throws(()=>logic.normalizeControlProfile({...controlProfile(),profile_version:4}));
  const missing=controlProfile();delete missing.consecutive_late_frame_limit;
  assert.throws(()=>logic.normalizeControlProfile(missing));
});

test('delayed degradation cannot clear input accepted later through the native vehicle result', () => {
  assert.equal(logic.degradedInputRearmRequired({rearm_required:false}, 0), false);
  assert.equal(logic.degradedInputRearmRequired({rearm_required:true,last_valid_command_seq:39}, 40), false);
  assert.equal(logic.degradedInputRearmRequired({rearm_required:true,last_valid_command_seq:40}, 40), true);
  assert.equal(logic.degradedInputRearmRequired({rearm_required:true,last_valid_command_seq:41}, 40), true);
  for(const timing of [undefined,{}, {last_valid_command_seq:-1}, {last_valid_command_seq:'39'}])
    assert.equal(logic.degradedInputRearmRequired(timing, 40), true);
});

test('profile equality checks every configurable field, independent of JSON order', () => {
  const profile=controlProfile();
  assert(logic.controlProfilesEqual(profile,Object.fromEntries(Object.entries(profile).reverse())));
  for(const field of ['consecutive_late_frame_limit','parking_idle_timeout_ms','critical_camera_stale_brake_ms','control_recovery_timeout_ms'])
    assert.equal(logic.controlProfilesEqual(profile,{...profile,[field]:profile[field]+1}),false);
});

test('session control profile ACK only activates the matching pending request', () => {
  const requested = controlProfile({target_speed_kph: 2, max_motor_torque_nm: 100});
  const pending = {
    requestedProfile: requested,
    pendingRequestSeq: 7,
    effectiveProfile: null,
    effectiveRequestSeq: 0,
    acknowledged: false,
  };
  const stale = logic.reduceControlProfileStatus(pending, {
    active: true, accepted: true, last_request_seq: 6, effective_profile: requested,
  });
  assert.equal(stale.matched, false);
  assert.equal(stale.acknowledged, false);
  assert.equal(stale.pendingRequestSeq, 7);

  const mismatched = logic.reduceControlProfileStatus(pending, {
    active: true,
    accepted: true,
    reason: 'accepted',
    last_request_seq: 7,
    applied_revision: 7,
    effective_profile: {...requested, max_motor_torque_nm: 80},
  });
  assert.equal(mismatched.matched, true);
  assert.equal(mismatched.acknowledged, false);
  assert.equal(mismatched.invalidated, true);
  assert.equal(mismatched.reason, 'effective_profile_mismatch');

  for(const field of ['consecutive_late_frame_limit','parking_idle_timeout_ms','critical_camera_stale_brake_ms','control_recovery_timeout_ms']) {
    const result=logic.reduceControlProfileStatus(pending,{active:true,accepted:true,last_request_seq:7,applied_revision:7,
      effective_profile:{...requested,[field]:requested[field]+1}});
    assert.equal(result.acknowledged,false);
    assert.equal(result.reason,'effective_profile_mismatch');
  }

  const missingRevision = logic.reduceControlProfileStatus(pending, {
    active: true, accepted: true, last_request_seq: 7, effective_profile: requested,
  });
  assert.equal(missingRevision.acknowledged, false);
  assert.equal(missingRevision.reason, 'applied_revision_mismatch');

  const effective = {...requested};
  const accepted = logic.reduceControlProfileStatus(pending, {
    active: true,
    accepted: true,
    reason: 'accepted',
    last_request_seq: 7,
    applied_revision: 7,
    effective_profile: effective,
  });
  assert.equal(accepted.matched, true);
  assert.equal(accepted.acknowledged, true);
  assert.equal(accepted.pendingRequestSeq, 0);
  assert.equal(accepted.effectiveRequestSeq, 7);
  assert.equal(accepted.effectiveAppliedRevision, 7);
  assert.deepEqual(accepted.effectiveProfile, effective);

  const prior = {...pending, effectiveProfile: effective, effectiveRequestSeq: 5, acknowledged: true};
  const rejected = logic.reduceControlProfileStatus(prior, {
    event: 'session_control_profile_status',
    control_status_seq: 12,
    active: false,
    accepted: false,
    reason: 'vehicle_not_parked',
    last_request_seq: 7,
    effective_profile: null,
    session_control_profile: {
      active: true,
      accepted: true,
      last_request_seq: 5,
      effective_profile: effective,
    },
  });
  assert.equal(rejected.matched, true);
  assert.equal(rejected.acknowledged, false);
  assert.equal(rejected.effectiveProfile, null);
  assert.equal(rejected.effectiveRequestSeq, 0);
  assert.equal(rejected.invalidated, true);
  assert.equal(rejected.reason, 'vehicle_not_parked');
});

test('session control profile telemetry revokes stale or inactive effective state', () => {
  const effective = controlProfile({target_speed_kph: 2, max_motor_torque_nm: 100});
  const active = {
    requestedProfile: effective,
    pendingRequestSeq: 0,
    effectiveProfile: effective,
    effectiveRequestSeq: 7,
    effectiveAppliedRevision: 7,
    acknowledged: true,
  };
  const refreshed = logic.reduceControlProfileStatus(active, {
    active: true, accepted: true, last_request_seq: 7, applied_revision: 7,
    effective_profile: effective,
  });
  assert.equal(refreshed.acknowledged, true);
  assert.equal(refreshed.invalidated, false);
  assert.deepEqual(refreshed.effectiveProfile, effective);

  for (const status of [
    {active: false, accepted: false, last_request_seq: 7, applied_revision: 7,
      effective_profile: effective},
    {active: true, accepted: true, last_request_seq: 8, applied_revision: 7,
      effective_profile: effective},
    {active: true, accepted: true, last_request_seq: 7, applied_revision: 8,
      effective_profile: effective},
    {active: true, accepted: true, last_request_seq: 7, applied_revision: 7,
      effective_profile: {...effective, max_motor_torque_nm: 90}},
    {active: true, accepted: true, last_request_seq: 7, applied_revision: 7,
      effective_profile: {...effective, speed_pid_kd: 0.25}},
  ]) {
    const revoked = logic.reduceControlProfileStatus(active, status);
    assert.equal(revoked.acknowledged, false);
    assert.equal(revoked.invalidated, true);
    assert.equal(revoked.effectiveProfile, null);
  }
});

test('zero maximum brake pressure disables analog, service, and hard brake scalars', () => {
  const base = {
    gamepad: {steering: 0.5, throttle: 0.5, brake: 1},
    selectedGear: 'D',
    limits: {maxThrottle: 0.05, maxBrakePressureBar: 0,
      serviceBrakePressureBar: 0, hardBrakePressureBar: 0, maxSteeringDeg: 3},
    steeringFullScaleDeg: 30,
  };
  for (const keyState of [{}, {service_brake: true}, {hard_brake: true}]) {
    const control = logic.deriveControl({...base, keyState});
    assert.equal(control.throttle, 0);
    assert.equal(control.brake, 0);
    assert.equal(control.steering, 0.05);
  }
});

test('recoverable braking banner uses vehicle feedback and identifies the cause', () => {
  for (const [cause, text] of [['critical_video', /关键视频/], ['time_sync', /时间同步/], ['input_lease', /刷新过期/], ['control_link', /ACK 确认/]]) {
    const telemetry = {safety_state: 'CONTROL_ACTIVE', control_timing: {control_braking_wait: true, control_recovery_cause: cause}};
    const presentation = logic.deriveControlRecoveryPresentation(true, telemetry, true, false);
    assert.equal(presentation.visible, true);
    assert.equal(presentation.severity, 'warn');
    assert.match(presentation.banner, text);
    assert.match(presentation.banner, /会话保留/);
    assert.doesNotMatch(presentation.banner, /已停稳|重新连接/);
  }
  const delayed = logic.deriveControlRecoveryPresentation(true, {safety_state: 'DEGRADED'}, true, false);
  assert.equal(delayed.cause, 'control_delay');
  assert.match(delayed.banner, /控制指令延迟/);
  assert.match(delayed.banner, /控制暂缓.*牵引已撤销/);
  assert.doesNotMatch(delayed.banner, /制动等待/);
  assert.equal(logic.deriveControlRecoveryPresentation(true, {safety_state: 'CONTROL_ACTIVE', control_timing: {}}, true, false).visible, false);
});

test('yellow braking feedback yields to hard stops and marks stale confirmation', () => {
  const telemetry = {safety_state: 'DEGRADED', control_timing: {control_braking_wait: true, control_recovery_cause: 'time_sync'}};
  assert.equal(logic.deriveControlRecoveryPresentation(true, telemetry, true, true).visible, false);
  assert.equal(logic.deriveControlRecoveryPresentation(false, telemetry, true, false).visible, false);
  for (const safety_state of ['ESTOP', 'FAULT', 'TIMEOUT_BRAKE']) {
    assert.equal(logic.deriveControlRecoveryPresentation(true, {...telemetry, safety_state}, true, false).visible, false);
  }
  assert.equal(logic.deriveControlRecoveryPresentation(true, {...telemetry, estop: true}, true, false).visible, false);
  const stale = logic.deriveControlRecoveryPresentation(true, telemetry, false, false);
  assert.equal(stale.visible, true);
  assert.match(stale.banner, /此前已报告.*反馈已过期.*待确认/);
});

test('recovered brake hold stays visible until fresh motion releases it', () => {
  const telemetry = {safety_state: 'CONTROL_ACTIVE', control_timing: {control_braking_wait: false, recovery_brake_hold: true, control_recovery_cause: 'critical_video'}};
  assert.match(logic.deriveControlRecoveryPresentation(true, telemetry, true, false).banner, /链路已恢复.*制动保持.*重新按下/);
  assert.match(logic.deriveControlRecoveryPresentation(true, {...telemetry, safety_state: 'DEGRADED'}, true, false).banner, /链路已恢复.*制动保持/);
  telemetry.control_timing.recovery_brake_hold = false;
  assert.equal(logic.deriveControlRecoveryPresentation(true, telemetry, true, false).visible, false);
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

  const pageDisconnect = logic.deriveEstopPresentation(
      false, true, 'page_disconnect', 'vcu_handshake_disconnect');
  assert.equal(pageDisconnect.kind, 'page_disconnect');
  assert.equal(pageDisconnect.severity, 'warn');
  assert.match(pageDisconnect.banner, /重新确认参数并申请握手/);
  assert.doesNotMatch(pageDisconnect.banner, /不是由本页面请求/);

  const pageRequest = logic.deriveEstopPresentation(
      false, true, 'page_request', 'operator_estop');
  assert.equal(pageRequest.kind, 'page_request');
  assert.match(pageRequest.banner, /控制页面急停请求/);
  assert.doesNotMatch(pageRequest.banner, /不是由本页面请求/);

  const sessionLoss = logic.deriveEstopPresentation(
      false, true, 'session_loss', 'control_session_closed');
  assert.equal(sessionLoss.kind, 'session_loss');
  assert.match(sessionLoss.banner, /控制会话丢失/);

  const watchdog = logic.deriveEstopPresentation(
      false, true, 'watchdog', 'control_apply_timeout');
  assert.equal(watchdog.kind, 'watchdog');
  assert.match(watchdog.banner, /watchdog/);

  const softwareFault = logic.deriveEstopPresentation(
      false, true, 'software_fault', 'can_receive_failed');
  assert.equal(softwareFault.kind, 'software_fault');
  assert.match(softwareFault.banner, /软件故障/);

  const physicalEstop = logic.deriveEstopPresentation(
      false, true, 'physical_estop', 'physical_emergency_switch');
  assert.equal(physicalEstop.kind, 'physical_estop');
  assert.match(physicalEstop.banner, /物理急停/);

  const physicalOverridesLocal = logic.deriveEstopPresentation(
      true, true, 'physical_estop', 'physical_emergency_switch');
  assert.equal(physicalOverridesLocal.kind, 'physical_estop');
  assert.match(physicalOverridesLocal.banner, /物理急停/);
  assert.doesNotMatch(physicalOverridesLocal.banner, /本页面急停/);

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

test('control prepare deadline reserves at least half the command-gap budget', () => {
  assert.equal(logic.controlPrepareDeadlineMs(200), 100);
  assert.equal(logic.controlPrepareDeadlineMs(150), 75);
  assert.equal(logic.controlPrepareDeadlineMs(500), 100);
  assert.equal(logic.controlPrepareDeadlineMs(undefined), 100);
});

test('latest-write queue bounds heartbeat backlog and preserves a pending ESTOP', async () => {
  const deferred = [];
  const calls = [];
  const writeControl = (extra, announceUnavailable) => {
    calls.push({extra, announceUnavailable});
    return new Promise((resolve, reject) => deferred.push({resolve, reject}));
  };
  const unhandled = [];
  let urgentWrites = 0;
  const queue = logic.createLatestControlWriteQueue(
      writeControl,
      error => unhandled.push(error),
      () => {
        urgentWrites += 1;
        if (deferred[0]) {
          deferred[0].resolve({sent: false, reason: 'control_prepare_preempted_by_estop'});
        }
      });

  const first = queue.send({throttle: 0.4}, false);
  assert.equal(calls.length, 1);
  assert.equal(queue.enqueueHeartbeat(), true);
  assert.equal(queue.enqueueHeartbeat(), false);
  const estop = queue.send({estop: true}, true);
  assert.equal(urgentWrites, 1);

  await Promise.resolve();
  await Promise.resolve();
  assert.equal(calls.length, 2);
  assert.deepEqual(calls[1], {extra: {estop: true}, announceUnavailable: true});
  assert.equal(queue.enqueueHeartbeat(), true);
  assert.equal(queue.enqueueHeartbeat(), false);

  deferred[1].resolve({id: 2});
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(calls.length, 3);
  deferred[2].resolve({id: 3});
  assert.deepEqual(await first, {sent: false, reason: 'control_prepare_preempted_by_estop'});
  assert.deepEqual(await estop, {id: 2});
  await Promise.resolve();
  assert.deepEqual(unhandled, []);
});

test('waiterless heartbeat rejection is consumed and the queue continues', async () => {
  const failure = new Error('synthetic heartbeat failure');
  const calls = [];
  const unhandled = [];
  const queue = logic.createLatestControlWriteQueue(
      async extra => {
        calls.push(extra);
        if (calls.length === 1) throw failure;
        return {sent: true};
      },
      error => unhandled.push(error));

  assert.equal(queue.enqueueHeartbeat(), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(unhandled, [failure]);
  assert.equal(queue.enqueueHeartbeat(), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(calls.length, 2);
});

test('every prepared browser command has exactly one named terminal outcome', () => {
  assert.equal(logic.shouldLogControlOutcome('prepared'), false);
  assert.equal(logic.shouldLogControlOutcome('forwarded'), false);
  assert.equal(logic.shouldLogControlOutcome('superseded'), false);
  assert.equal(logic.shouldLogControlOutcome('expired_before_forward'), false);
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
    prepared: 5,
    forwarded: 1,
    superseded: 1,
    expired_before_forward: 1,
    post_prepare_link_changed: 1,
    post_prepare_vcu_not_ready: 1,
    last_prepared_seq: 14,
    last_forwarded_seq: 10,
  });
  assert.throws(
      () => logic.reduceControlOutcome(metrics, 'forwarded', 14),
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

test('vehicle startup and camera inhibition diagnostics remain distinct from CAN faults', () => {
  const pending = logic.vehicleControlDiagnostic('critical_camera_not_ready');
  assert.equal(pending.level, 'warn');
  assert.match(pending.text, /尚未启动/);
  const inhibited = logic.vehicleControlDiagnostic('critical_camera_failed');
  assert.equal(inhibited.level, 'critical');
  assert.match(inhibited.text, /画面恢复不会解除锁定/);
  assert.match(inhibited.text, /结束会话/);
  assert.match(logic.vehicleControlDiagnostic('vcu_adapter_start_failed').text, /CAN 配置/);
  assert.equal(logic.vehicleControlDiagnostic(''), null);
  assert.equal(logic.vehicleControlDiagnostic('future-code'), null);
});

test('video protection timings reject missing, old, fractional and out-of-range values', () => {
  for(const field of ['critical_camera_stale_brake_ms','control_recovery_timeout_ms']){
    const missing=controlProfile();delete missing[field];assert.throws(()=>logic.normalizeControlProfile(missing));
    for(const invalid of [0, 0.5, 30001])assert.throws(()=>logic.normalizeControlProfile({...controlProfile(),[field]:invalid}));
  }
  assert.throws(()=>logic.normalizeControlProfile({...controlProfile(),profile_version:5}));
  assert.equal(logic.normalizeControlProfile(controlProfile()).control_recovery_timeout_ms,5000);
  assert.match(logic.vehicleControlDiagnostic('control_recovery_braking_wait').text,/当前会话与参数保留/);
});

test('shipped view health distinguishes optional-only cameras from unknown critical configuration', () => {
  const context=vm.createContext({mediaStatus:{lanes:[]},controlProfileState:{effectiveProfile:null},
    vehicleHardLimits:{default_critical_camera_stale_brake_ms:700},
    cameraView:{criticalHealth:(ids,staleMs)=>({ready:ids.length>0,missing:ids,staleMs})}});
  const handler=controlPageSource.match(/^function driverCriticalVideoHealth\(\)\{[\s\S]*?^\}/m);
  assert(handler);vm.runInContext(handler[0],context);
  assert.equal(context.driverCriticalVideoHealth().ready,false);
  context.mediaStatus={lanes:[{camera_id:'aux',critical_for_control:false}]};
  assert.equal(context.driverCriticalVideoHealth().ready,true);
  context.mediaStatus={lanes:[{camera_id:'unknown'}]};
  assert.equal(context.driverCriticalVideoHealth().ready,false);
  context.mediaStatus={lanes:[{camera_id:'front',critical_for_control:true}]};
  assert.equal(context.driverCriticalVideoHealth().staleMs,700);
  context.controlProfileState.effectiveProfile={critical_camera_stale_brake_ms:900};
  assert.equal(context.driverCriticalVideoHealth().staleMs,900);
});

test('untrusted driver time sends neutral and retains the session until fresh input', async () => {
  const fixture=intentPageFixture();
  fixture.context.driverTimeSyncReady=()=>false;
  const request=fixture.context.writeControlIntent({},true);
  assert.equal(fixture.requests[0].intent.throttle,0);
  assert.equal(fixture.requests[0].intent.steering,0);
  assert.equal(fixture.context.controlProfileState.acknowledged,true);
  assert.equal(fixture.context.controlAuthorityLost,false);
  fixture.requests[0].resolve({accepted:true});await request;
  fixture.context.driverTimeSyncReady=()=>true;
  const recovered=fixture.context.writeControlIntent({},false);
  assert.equal(fixture.requests[1].intent.throttle,0);
  fixture.requests[1].resolve({accepted:true});await recovered;
  assert(fixture.blocked.has('ArrowUp'));
});

test('lost critical view sends neutral without disconnecting or clearing the profile', async () => {
  const fixture=intentPageFixture();
  fixture.context.driverCriticalVideoHealth=()=>({ready:false,missing:['front']});
  const request=fixture.context.writeControlIntent({},true);
  assert.equal(fixture.requests.length,1);
  assert.equal(fixture.requests[0].intent.throttle,0);
  assert.equal(fixture.requests[0].intent.steering,0);
  assert.equal(fixture.context.controlProfileState.acknowledged,true);
  assert.equal(fixture.context.controlAuthorityLost,false);
  assert(fixture.blocked.has('ArrowUp'));
  fixture.requests[0].resolve({accepted:true});await request;
  fixture.context.driverCriticalVideoHealth=()=>({ready:true,missing:[]});
  const recovered=fixture.context.writeControlIntent({},false);
  assert.equal(fixture.requests[1].intent.throttle,0);
  assert.equal(fixture.requests[1].intent.steering,0);
  fixture.requests[1].resolve({accepted:true});await recovered;
});

test('hard stop banner retains exact recovery cause and deadline without telemetry', () => {
  for (const [cause, label] of [['time_sync',/时间同步/], ['input_lease',/输入刷新/], ['control_link',/ACK/]]) {
    const handshake={issue_code:'control_recovery_timeout',control_recovery:{cause,timeout_ms:5000,phase:'locked'}};
    const p=logic.deriveControlStopPresentation(false,false,'','',handshake);
    assert.equal(p.visible,true);assert.equal(p.severity,'critical');assert.match(p.banner,label);
    assert.match(p.banner,/5 秒/);assert.match(p.banner,/控制已锁定/);
    assert.equal(logic.deriveControlStopPresentation(false,false,'','',{}).visible,false);
    assert.equal(logic.deriveControlStopPresentation(true,false,'','',handshake).kind,'local_pending');
    assert.equal(logic.deriveControlStopPresentation(false,true,'physical_estop','',handshake).kind,'physical_estop');
  }
  const video=logic.deriveControlStopPresentation(false,false,'','',{issue_code:'critical_camera_failed'});
  assert.match(video.banner,/关键视频/);
});

test('signaling fault status locks motion and preserves the reason before DataChannel feedback', () => {
  const context=vm.createContext({mediaStatus:{lanes:[]},vcuHandshake:{ready:true,state:'ready'},controlLogic:logic,
    renderMonitoring(){},updateVcuHandshakeState(value){context.vcuHandshake=value}});
  const handler=controlPageSource.match(/^function applyMediaStatus\(payload\)\{[\s\S]*?^\}/m);
  assert(handler);vm.runInContext(handler[0],context);
  context.applyMediaStatus({control_issue_code:'control_recovery_braking_wait',control_recovery:{cause:'time_sync',timeout_ms:5000}});
  assert.equal(context.vcuHandshake.ready,true);
  context.applyMediaStatus({control_issue_code:'control_recovery_timeout',control_recovery:{cause:'time_sync',timeout_ms:5000}});
  assert.equal(context.vcuHandshake.ready,false);assert.equal(context.vcuHandshake.state,'fault');
  assert.match(logic.deriveControlStopPresentation(false,false,'','',context.vcuHandshake).banner,/时间同步.*5 秒/);
  context.applyMediaStatus({control_issue_code:''});assert.equal(context.vcuHandshake.ready,false);
});

test('delayed good DataChannel status and older soft signal cannot erase a hard stop', () => {
  const context=vm.createContext({mediaStatus:{lanes:[]},vcuHandshake:{ready:true,state:'ready'},controlLogic:logic,
    gearChangeStationaryEvidence:logic.createGearChangeStationaryEvidence(),lastControlStatusSeq:0,vcuEverReady:true,
    performance:{now:()=>1000},renderMonitoring(){},resetControlAuthorityInput(){}});
  for(const name of ['updateVcuHandshakeState','applyMediaStatus']) {
    const handler=controlPageSource.match(/^function applyMediaStatus\(payload\)\{[\s\S]*?^\}/m);
    const declaration=name==='updateVcuHandshakeState'?controlPageSource.split('\n').find(x=>x.startsWith('function '+name+'(')):handler&&handler[0];
    assert(declaration);vm.runInContext(declaration,context);
  }
  context.applyMediaStatus({control_issue_code:'control_recovery_timeout',control_recovery:{cause:'time_sync',timeout_ms:5000}});
  context.updateVcuHandshakeState({ready:true,state:'ready',issue_code:'',adapter_ready:true});
  assert.equal(context.vcuHandshake.ready,false);assert.equal(context.vcuHandshake.issue_code,'control_recovery_timeout');
  context.applyMediaStatus({control_issue_code:'control_recovery_braking_wait',control_recovery:{cause:'critical_video',timeout_ms:10000}});
  assert.equal(context.vcuHandshake.issue_code,'control_recovery_timeout');assert.equal(context.vcuHandshake.control_recovery.cause,'time_sync');
  assert.match(logic.deriveControlStopPresentation(false,false,'','',context.vcuHandshake).banner,/时间同步.*5 秒/);
});

Promise.all(pendingTests).then(() => {
  if (failed > 0) {
    console.error(`${failed} of ${passed + failed} control logic tests failed`);
    process.exitCode = 1;
  } else {
    console.log(`${passed} control logic tests passed`);
  }
});
