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
const unfinishedTests = new Set();
class OfflineAbortController {
  constructor() {
    const listeners=new Set();this.listeners=listeners;
    this.signal={aborted:false,reason:undefined,onabort:null,
      addEventListener(type,callback){if(type==='abort')listeners.add(callback)},
      removeEventListener(type,callback){if(type==='abort')listeners.delete(callback)}};
  }
  abort(reason) {
    if(this.signal.aborted)return;
    this.signal.aborted=true;this.signal.reason=reason===undefined?Object.assign(new Error('request was aborted'),{name:'AbortError'}):reason;
    const event={type:'abort',target:this.signal};
    if(this.signal.onAbort)this.signal.onAbort(event);
    if(this.signal.onabort)this.signal.onabort(event);
    for(const callback of this.listeners)callback(event);
  }
}
function test(name, body) {
  function pass() {
    unfinishedTests.delete(name);
    passed += 1;
    console.log(`[PASS] ${name}`);
  }
  function fail(error) {
    unfinishedTests.delete(name);
    failed += 1;
    console.error(`[FAIL] ${name}`);
    console.error(error && error.stack ? error.stack : error);
  }
  try {
    const result = body();
    if (result && typeof result.then === 'function') {
      unfinishedTests.add(name);
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
  const timerDeadlines = new Map();
  const requests = [];
  let timerId = 0;
  let elapsed = 1000;
  const context = {
    controlLogic: logic,
    peer: {connectionState: 'connected'},
    controlChannel: {readyState: 'open'},
    controlAuthorityLost: false,
    safeLogoutAttempt: null,
    gearRejectionInhibited: false,
    controlProfileState: {acknowledged: true},
    vcuHandshake: readyVcu,
    vehicleTelemetry: null,
    driverCriticalVideoHealth: () => ({ready: true, sample_healthy: true, missing: []}),
    driverTimeSyncReady: () => true,
    latestRuntimeStatus: {time_sync_readiness: {ready: true, latest_candidate_healthy: true}},
    pendingGearTransition: null,
    estopLatched: false,
    activeControlRequests: new Map(), controlTraceEnabled: false, nativeIntentSeq: 62,
    nativeControlSessionId: 'test-session',
    nativeControlSessionGeneration: 1,
    nativeControlUiEpoch: 1, nativeControlTakeoverPending: false, nativeControlPageRetired: false,
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
      intent_seq: context.nativeIntentSeq,
    }, outgoing),
    controlInputDiagnostic: () => ({pressed_codes: Array.from(pressed), blocked_codes: Array.from(blocked)}),
    clearControlInput: (resetGear, reason) => {
      logic.blockAndClearKeys(pressed, blocked);
      logs.push({event: 'cleared', reason});
    },
    resetControlAuthorityInput: () => { context.controlAuthorityLost = true; logic.blockAndClearKeys(pressed, blocked); },
    resetControlProfileSession: () => { context.controlProfileState.acknowledged = false; },
    clientLog: (event, details) => logs.push({event, details}),
    setTimeout: (callback, delay) => { const id = ++timerId; timers.set(id, callback); timerDeadlines.set(id, elapsed + delay); return id; },
    clearTimeout: id => { timers.delete(id); timerDeadlines.delete(id); },
    AbortController: OfflineAbortController,
    post: (url, intent, signal) => new Promise((resolve, reject) => {
      signal.onAbort = () => reject(Object.assign(new Error('delayed local response'), {name: 'AbortError'}));
      requests.push({url, intent, resolve, reject, startedAt: elapsed});
    }),
  };
  vm.createContext(context);
  const clockGateStart = controlPageSource.indexOf('function driverControlTimeSyncReady(');
  const clockGateEnd = controlPageSource.indexOf('function nativeIntentEnvelope(', clockGateStart);
  assert.ok(clockGateStart >= 0 && clockGateEnd > clockGateStart);
  vm.runInContext(controlPageSource.slice(clockGateStart, clockGateEnd), context);
  const start = controlPageSource.indexOf('async function writeControlIntent(');
  const end = controlPageSource.indexOf('function reportControlQueueError(', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(controlPageSource.slice(start, end), context);
  return {context, pressed, blocked, logs, requests,
    advanceTo: async (now, delayedCallbacks = false) => {
      if (delayedCallbacks) elapsed = now;
      for (;;) {
        const due = Array.from(timerDeadlines.entries()).filter(([, time]) => time <= now)
          .sort((a, b) => a[1] - b[1])[0];
        if (!due) break;
        if (!delayedCallbacks) elapsed = due[1];
        const callback = timers.get(due[0]);
        timerDeadlines.delete(due[0]); timers.delete(due[0]);
        callback();
        await new Promise(resolve => setImmediate(resolve));
      }
      elapsed = now;
    },
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
  Array.from(fixture.context.activeControlRequests.values())[0].preemptedReason = 'estop';
  fixture.context.estopLatched = true;
  fixture.timeout();
  assert.equal((await urgent).reason, 'control_intent_preempted_estop');
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
          "return{session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_instance_id:uiInstanceId,intent_seq:nativeIntentSeq,ui_control_epoch:nativeControlUiEpoch,takeover:nativeControlTakeoverPending&&!normalized.estop,...normalized}"),
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

test('vehicle catalog displays an incompatible version cause instead of online control readiness', () => {
  const options=[];
  const context=vm.createContext({latestRuntimeStatus:{connected:false},connecting:false,connectButton:{},
    vehicleSelect:{value:'',replaceChildren(){options.length=0;},appendChild(value){options.push(value);}},
    document:{createElement(){return {};}}});
  const start=controlPageSource.indexOf('function renderVehicles('),end=controlPageSource.indexOf('function renderAuthExpiry(',start);
  assert.ok(start>=0&&end>start);vm.runInContext(controlPageSource.slice(start,end),context);
  context.renderVehicles([{vehicle_id:'old-vehicle',state:'online',controllable:false,native_control_compatible:false},
    {vehicle_id:'precise-vehicle',state:'online',controllable:false,native_control_compatible:false,control_issue_message:'车端协议能力不匹配，请更新'},
    {vehicle_id:'current-vehicle',state:'online',controllable:true,native_control_compatible:true}]);
  assert.equal(options[0].disabled,true);assert.match(options[0].textContent,/版本不兼容.*更新/);
  assert.doesNotMatch(options[0].textContent,/在线可控/);
  assert.equal(options[1].textContent,'precise-vehicle · 车端协议能力不匹配，请更新');
  assert.equal(options[2].disabled,false);assert.match(options[2].textContent,/在线可控/);
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

test('media ICE, cloud ingress and correlated vehicle results remain independent in the shipped monitor', () => {
  const runtime={connected:true,session_id:'s',control_session_generation:7,native_control:{session_generation:7,
    last_ack_seq:3930,last_ack_packet_age_ms:59,last_ack_receive_age_ms:10,ack_stall_age_ms:0,
    vehicle_result_latency:{command_seq:3929,accepted:false,round_trip_ms:310,result_age_ms:20,
      last_accepted_command_seq:3927,last_accepted_result_age_ms:2037}}};
  let view=logic.controlLatencyPresentation({media_transport_rtt_ms:3,control_rtt_ms:3},runtime,'s',7);
  assert.equal(view.media.text,'3 ms');assert.match(view.cloud.text,/#3930/);
  assert.match(view.vehicle.text,/#3929.*软件拒绝.*310 ms/);
  assert.match(view.accepted.text,/#3927.*2037 ms/);
  assert(!JSON.stringify(view).includes('已执行'));
  runtime.native_control.last_ack_seq=4000;
  view=logic.controlLatencyPresentation({control_rtt_ms:1},runtime,'s',7);
  assert.equal(view.media.text,'未知','legacy control_rtt field still masquerades as media/control measurement');
  assert.match(view.accepted.text,/#3927.*2037 ms/,'fresh cloud ACK refreshed vehicle acceptance');
  for(const [session,generation] of [['old',7],['s',8]]) {
    const stale=logic.controlLatencyPresentation({media_transport_rtt_ms:3},runtime,session,generation);
    assert.equal(stale.cloud.text,'尚无入口确认');assert.equal(stale.vehicle.text,'尚无关联结果');
  }
  runtime.native_control.vehicle_result_latency={command_seq:9,accepted:true,round_trip_ms:null,result_age_ms:null};
  view=logic.controlLatencyPresentation({},runtime,'s',7);
  assert.match(view.vehicle.text,/往返 未知.*收到后 未知/);
  assert.equal(view.accepted.text,'尚无关联接受结果');
  assert(controlPageSource.includes('<span>媒体 ICE RTT</span>'));
  assert(controlPageSource.includes("setMetric('metric-cloud-ack',latency.cloud.text"));
  assert(controlPageSource.includes("setMetric('metric-vehicle-result',latency.vehicle.text"));
  assert(controlPageSource.includes('control_rtt_ms:null'));
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
  for (const [cause, text] of [['critical_video', /关键视频/], ['time_sync', /时间同步/], ['input_lease', /输入刷新/], ['control_link', /控制命令/]]) {
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
  const telemetry = {safety_state: 'CONTROL_ACTIVE', control_timing: {control_braking_wait: false, recovery_brake_hold: true, control_recovery_cause: 'critical_video', recovery_samples_healthy: true}};
  assert.match(logic.deriveControlRecoveryPresentation(true, telemetry, true, false).banner, /制动保持.*释放.*重新按下/);
  assert.match(logic.deriveControlRecoveryPresentation(true, {...telemetry, safety_state: 'DEGRADED'}, true, false).banner, /制动保持.*下一步/);
  for (const recovery_samples_healthy of [false, undefined]) {
    const pending = {...telemetry, control_timing: {...telemetry.control_timing, recovery_samples_healthy}};
    const banner = logic.deriveControlRecoveryPresentation(true, pending, true, false).banner;
    assert.match(banner, /当前阻挡.*等待/);
    assert.doesNotMatch(banner, /链路已恢复|重新按下/);
  }
  telemetry.control_timing.recovery_brake_hold = false;
  assert.equal(logic.deriveControlRecoveryPresentation(true, telemetry, true, false).visible, false);
});

test('recovery guidance uses current four readiness causes rather than the first historical cause', () => {
  const fields = {critical_video: 'driver_critical_video_ready', input_lease: 'operator_input_ready',
    control_link: 'driver_control_link_ready', time_sync: 'driver_time_sync_ready'};
  const labels = {critical_video: /关键视频/, input_lease: /输入刷新/, control_link: /控制命令/, time_sync: /时间同步/};
  for (const [cause, field] of Object.entries(fields)) {
    const guidance = logic.controlRecoveryGuidance({control_braking_wait: true,
      control_recovery_cause: 'time_sync', recovery_samples_healthy: false, [field]: false});
    assert.equal(guidance.phase, 'wait_conditions');
    assert.deepEqual(guidance.causes, [cause]);
    assert.match(guidance.reason, labels[cause]);
    assert.match(guidance.text, /当前阻挡.*下一步/);
    assert.doesNotMatch(guidance.text, /已停稳|已经停车|立即行驶|重新连接/);
  }
  const optional = logic.controlRecoveryGuidance({control_braking_wait: true,
    critical_video_required: false, driver_critical_video_ready: false,
    recovery_samples_healthy: true});
  assert.deepEqual(optional.causes, [], 'non-critical video was relabeled as a control blocker');
  assert.equal(optional.phase, 'healthy_window');
});

test('healthy confirmation, fresh input and stale feedback have distinct recovery guidance', () => {
  const waiting = {control_braking_wait: true, recovery_brake_hold: true, rearm_required: true,
    recovery_samples_healthy: true, control_recovery_cause: 'critical_video'};
  const healthy = logic.controlRecoveryGuidance(waiting);
  assert.equal(healthy.phase, 'healthy_window');
  assert.match(healthy.nextStep, /300 ms.*后/);
  assert.doesNotMatch(healthy.reason, /已恢复|已就绪/);
  const rearm = logic.controlRecoveryGuidance({...waiting, control_braking_wait: false});
  assert.equal(rearm.phase, 'await_fresh_input');
  assert.match(rearm.nextStep, /释放.*重新按下/);
  const renewedFailure = logic.controlRecoveryGuidance({...waiting,
    control_braking_wait: false, recovery_samples_healthy: false});
  assert.equal(renewedFailure.phase, 'wait_conditions');
  assert.doesNotMatch(renewedFailure.nextStep, /重新按下/);
  const stale = logic.controlRecoveryGuidance({...waiting, control_braking_wait: false}, false);
  assert.equal(stale.phase, 'feedback_unknown');
  assert.match(stale.reason, /待确认/);
  assert.doesNotMatch(stale.nextStep, /重新按下/);
  assert.equal(logic.controlRecoveryGuidance({}).visible, false);
  assert.equal(logic.controlRecoveryGuidance(null).visible, false);
  const unknown = logic.controlRecoveryGuidance({control_braking_wait: true, control_recovery_cause: 'unexpected'});
  assert.match(unknown.reason, /尚未确认/);
  assert.doesNotMatch(unknown.reason, /unexpected/);
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

test('slow responses do not serialize heartbeats; backlog is bounded and ESTOP has a reserved slot', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  const queue = logic.createLatestControlWriteQueue(fixture.context.writeControlIntent,
    error => { throw error; }, (token,reason) => {
      const active = fixture.context.activeControlRequests.get(token);
      if (active && !active.estop) { active.preemptedReason = reason; active.controller.abort(); }
    });
  const first = queue.send({}, false);
  assert.equal(fixture.requests.length, 1);
  assert.equal(queue.enqueueHeartbeat(), true);
  assert.equal(fixture.requests.length, 2, 'live heartbeat waited behind slow response');
  // Two slow ordinary requests plus a single latest pending refresh; no replay backlog.
  assert.equal(queue.enqueueHeartbeat(), true);assert.equal(queue.enqueueHeartbeat(), false);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(fixture.requests.length, 2);
  assert.equal(fixture.context.activeControlRequests.size, 2);
  // A response between the third tick (132ms) and its own 150ms deadline
  // remains usable; the single waiting heartbeat dispatches on completion.
  fixture.requests[0].resolve({accepted:true,duplicate:true});
  assert.equal((await first).sent,true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(fixture.requests.length,3);
  assert.equal(fixture.pressed.has('ArrowUp'),true);
  logic.releaseKey(fixture.pressed, fixture.blocked, 'ArrowUp');fixture.context.nativeIntentSeq++;
  const release = queue.send({}, false);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(fixture.requests[3].intent.throttle,0);
  fixture.context.estopLatched=true;
  const urgent=queue.send({estop:true},true);
  assert.equal(fixture.requests.length,5,'ESTOP waited behind two ordinary responses');
  assert.equal(fixture.requests[4].intent.estop,true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal((await release).reason,'control_intent_preempted_estop');
  assert.equal(queue.enqueueHeartbeat(),true);
  assert.equal(fixture.requests.length,6);
  fixture.requests[5].resolve({accepted:true});
  await new Promise(resolve => setImmediate(resolve));
  // A heartbeat cannot abort the outstanding urgent request.
  assert.equal(fixture.context.activeControlRequests.size,1);
  fixture.requests[4].resolve({accepted:true});assert.equal((await urgent).sent,true);
});

test('production 66ms refresh and 150ms response deadlines stay bounded under slow HTTP and a 124.5ms timer lag', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  await fixture.advanceTo(0);
  const queue = logic.createLatestControlWriteQueue(fixture.context.writeControlIntent,
    error => { throw error; }, (token, reason) => {
      const active = fixture.context.activeControlRequests.get(token);
      if (active && !active.estop) { active.preemptedReason = reason; active.controller.abort(); }
    });
  const first = queue.send({}, false);
  for (const tick of [66, 132]) {
    await fixture.advanceTo(tick); queue.enqueueHeartbeat();
  }
  // Simulate the observed timer lag without executing browser callbacks in
  // the blocked interval. The native lease test separately proves the 200ms
  // boundary; this test measures actual production queue dispatch times.
  await fixture.advanceTo(256.5, true);
  assert.equal(fixture.context.activeControlRequests.size <= 2, true);
  for (let tick = 264; tick <= 660; tick += 66) {
    await fixture.advanceTo(tick); queue.enqueueHeartbeat();
    assert.equal(fixture.context.activeControlRequests.size <= 2, true);
  }
  assert.equal((await first).reason, 'control_intent_update_timeout');
  const times = fixture.requests.map(request => request.startedAt);
  const gaps = times.slice(1).map((time, index) => time - times[index]);
  assert.equal(Math.max(...gaps), 190.5);
  assert.equal(gaps.every(gap => gap < 200), true, 'request serialization exceeded the unchanged lease');
  assert.equal(fixture.pressed.has('ArrowUp'), true, 'response timeout manufactured a physical key release');
  await fixture.advanceTo(1000);
});

function useProductionIntentEnvelope(fixture) {
  fixture.context.uiInstanceId = 'page-under-test';
  const start = controlPageSource.indexOf('function nativeIntentEnvelope(');
  const end = controlPageSource.indexOf('async function writeControlIntent(', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(controlPageSource.slice(start, end), fixture.context);
}

test('a two-second page pause is distinct from slow HTTP responses and does not replay a catch-up queue', async () => {
  const fixture=intentPageFixture(['ArrowUp']);await fixture.advanceTo(0);
  useProductionIntentEnvelope(fixture);
  const queue=logic.createLatestControlWriteQueue(fixture.context.writeControlIntent,error=>{throw error;},
    (token,reason)=>{const active=fixture.context.activeControlRequests.get(token);if(active){active.preemptedReason=reason;active.controller.abort();}});
  const first=queue.send({},false);
  for(const tick of [66,132]){await fixture.advanceTo(tick);queue.enqueueHeartbeat();}
  await fixture.advanceTo(2316.7,true);
  assert.equal((await first).reason,'control_intent_update_timeout');
  assert.equal(fixture.requests.length,3,'pause accumulated a replay backlog');
  assert.equal(fixture.requests[2].startedAt-fixture.requests[1].startedAt,2250.7);
  assert.equal(fixture.pressed.has('ArrowUp'),true,'HTTP timeout fabricated a key release');
  // The native store, rather than the HTTP timeout, owns the unchanged lease.
  fixture.requests[2].resolve({accepted:false,reason:'fresh_neutral_required',requires_fresh_input:true});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(fixture.pressed.size,0);
  assert.equal(logic.pressKey(fixture.pressed,fixture.blocked,'ArrowUp').accepted,false);
  logic.releaseKey(fixture.pressed,fixture.blocked,'ArrowUp');
  assert.equal(logic.pressKey(fixture.pressed,fixture.blocked,'ArrowUp').accepted,true);
  queue.reset();await new Promise(resolve=>setImmediate(resolve));
});

function useProductionCameraView(fixture) {
  class Element {
    constructor(tag) { this.tag=tag; this.children=[]; this.dataset={}; this.frames=0; this.paused=false; this.readyState=4; }
    append(...items) { this.children.push(...items); }
    replaceChildren(...items) { this.children=items; }
    insertBefore(item, before) { this.children=this.children.filter(value=>value!==item); const index=this.children.indexOf(before); this.children.splice(index<0?this.children.length:index,0,item); }
    setAttribute() {}
    getVideoPlaybackQuality() { return {totalVideoFrames:this.frames,droppedVideoFrames:0}; }
  }
  fixture.context.document={createElement:tag=>new Element(tag)};
  fixture.context.MediaStream=class {};
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../web/camera_view.js'),'utf8'),fixture.context);
  const grid=new Element('grid'),view=fixture.context.MineTeleopCameraView.mount(grid),listeners=new Map();
  const track={muted:false,readyState:'live',
    addEventListener(name,callback) { if(!listeners.has(name))listeners.set(name,new Set());listeners.get(name).add(callback); },
    removeEventListener(name,callback) { const callbacks=listeners.get(name);if(callbacks)callbacks.delete(callback); },
    emit(name) { for(const callback of listeners.get(name)||[])callback(); }};
  view.configure(['front']);view.attach('front',track);
  const video=grid.children[0].children[1];
  fixture.context.driverCriticalVideoHealth=()=>view.criticalHealth(['front'],500);
  return {view,track,video};
}

test('actual CameraView and shipped writer tolerate two raw faults, propagate raw health and clear at the third', async () => {
  const fixture=intentPageFixture(['ArrowUp']);
  await fixture.advanceTo(0);
  const camera=useProductionCameraView(fixture);
  useProductionIntentEnvelope(fixture);
  for(const now of [100,133,166]) {
    await fixture.advanceTo(now);camera.video.frames++;fixture.context.driverCriticalVideoHealth();
  }
  assert.equal(fixture.context.driverCriticalVideoHealth().ready,true);
  for(const [index,now] of [200,220,240].entries()) {
    if(index) { await fixture.advanceTo(now-10);camera.track.muted=false;camera.track.emit('unmute'); }
    await fixture.advanceTo(now);camera.track.muted=true;camera.track.emit('mute');
    const health=fixture.context.driverCriticalVideoHealth();
    assert.equal(health.cameras[0].consecutive_anomalies,index+1);
    assert.equal(health.sample_healthy,false);
    const result=fixture.context.writeControlIntent({},false),request=fixture.requests[index];
    assert.equal(request.intent.critical_video_sample_healthy,false,'raw fault was silently relabeled normal for recovery');
    assert.equal(request.intent.critical_video_ready,index<2);
    assert.equal(request.intent.throttle,index<2?0.5:0);
    assert.equal(fixture.pressed.has('ArrowUp'),index<2,'writer bypassed or delayed the qualified video threshold');
    request.resolve({accepted:true});await result;
  }
  assert.equal(fixture.context.controlProfileState.acknowledged,true);
  assert.equal(fixture.context.controlAuthorityLost,false);
  await fixture.advanceTo(250);camera.track.muted=false;camera.track.emit('unmute');
  await fixture.advanceTo(260);camera.video.frames++;
  const recovered=fixture.context.writeControlIntent({},false);
  assert.equal(fixture.requests[3].intent.critical_video_sample_healthy,true);
  assert.equal(fixture.requests[3].intent.throttle,0,'new normal video resurrected an old physical long press');
  assert.equal(fixture.blocked.has('ArrowUp'),true);
  fixture.requests[3].resolve({accepted:true});await recovered;
});

test('writer uses the qualified clock gate while raw clock faults remain diagnostic', async () => {
  const fixture=intentPageFixture(['ArrowUp']);useProductionIntentEnvelope(fixture);
  fixture.context.driverTimeSyncReady=()=>false;
  for(const count of [1,2,3]) {
    fixture.context.latestRuntimeStatus.time_sync_readiness={ready:count<3,latest_candidate_healthy:false,consecutive_anomalies:count};
    const result=fixture.context.writeControlIntent({},false),request=fixture.requests[count-1];
    assert.equal(request.intent.throttle,count<3?0.5:0,'raw clock trust bypassed qualified control readiness');
    assert.equal(fixture.pressed.has('ArrowUp'),count<3);
    request.resolve({accepted:true});await result;
  }
  assert.equal(fixture.context.controlProfileState.acknowledged,true);
  assert.equal(fixture.context.controlAuthorityLost,false);
  const unknown=intentPageFixture(['ArrowUp']);
  unknown.context.latestRuntimeStatus.time_sync_readiness=undefined;
  const result=unknown.context.writeControlIntent({},false);
  assert.equal(unknown.requests[0].intent.throttle,0,'missing qualified clock metadata assumed readiness');
  unknown.requests[0].resolve({accepted:true});await result;
});

test('missing page metadata asks for reload without blocking a legal emergency request', async () => {
  const fixture=intentPageFixture(['ArrowUp']);
  const result=fixture.context.writeControlIntent({},false);
  fixture.requests[0].reject(Object.assign(new Error('refresh page'),{status:400,issue_code:'control_page_metadata_missing'}));
  assert.equal((await result).reason,'control_page_metadata_missing');
  assert.equal(fixture.pressed.size,0);
  assert.match(fixture.context.statusPanel.textContent,/当前阻挡.*刷新页面/);
  assert.equal(fixture.context.controlAuthorityLost,false);
  fixture.context.estopLatched=true;
  const urgent=fixture.context.writeControlIntent({estop:true},false);
  assert.equal(fixture.requests[1].intent.estop,true);
  fixture.requests[1].resolve({accepted:true});await urgent;
});

test('new page sends neutral until confirmed takeover and then requires physical release', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  useProductionIntentEnvelope(fixture);
  fixture.context.nativeControlUiEpoch = 7;
  fixture.context.nativeControlTakeoverPending = true;
  const handshake = fixture.context.writeControlIntent({}, false);
  const intent = fixture.requests[0].intent;
  assert.equal(intent.takeover, true);
  assert.equal(intent.ui_control_epoch, 7);
  assert.equal(intent.throttle, 0);
  assert.equal(fixture.blocked.has('ArrowUp'), true);
  fixture.requests[0].resolve({accepted: true, ui_control_epoch: 8, intent_seq: intent.intent_seq});
  assert.equal((await handshake).sent, true);
  assert.equal(fixture.context.nativeControlUiEpoch, 8);
  assert.equal(fixture.context.nativeControlTakeoverPending, false);
  assert.equal(logic.pressKey(fixture.pressed, fixture.blocked, 'ArrowUp').changed, false);
  logic.releaseKey(fixture.pressed, fixture.blocked, 'ArrowUp');
  logic.pressKey(fixture.pressed, fixture.blocked, 'ArrowUp');
  const drive = fixture.context.writeControlIntent({}, false);
  assert.equal(fixture.requests[1].intent.takeover, false);
  assert.equal(fixture.requests[1].intent.ui_control_epoch, 8);
  assert.equal(fixture.requests[1].intent.throttle, 0.5);
  fixture.requests[1].resolve({accepted: true, intent_seq: fixture.requests[1].intent.intent_seq});
  await drive;
});

test('Abort after server acceptance retries the neutral handshake without reviving held motion', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  useProductionIntentEnvelope(fixture);
  fixture.context.nativeControlUiEpoch = 3;
  fixture.context.nativeControlTakeoverPending = true;
  const first = fixture.context.writeControlIntent({}, false);
  // Acceptance is unknowable after response cancellation. Retry the same
  // epoch with neutral input; production Store accepts this owner idempotently.
  fixture.timeout();
  assert.equal((await first).reason, 'control_intent_update_timeout');
  assert.equal(fixture.context.nativeControlTakeoverPending, true);
  const retry = fixture.context.writeControlIntent({}, false);
  assert.equal(fixture.requests[1].intent.ui_control_epoch, 3);
  assert.equal(fixture.requests[1].intent.throttle, 0);
  fixture.requests[1].resolve({accepted: true, duplicate: true, ui_control_epoch: 4});
  assert.equal((await retry).sent, true);
  assert.equal(fixture.context.nativeControlUiEpoch, 4);
  assert.equal(fixture.blocked.has('ArrowUp'), true);
});

test('reconnecting the current page confirms its existing epoch without motion', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  useProductionIntentEnvelope(fixture);
  fixture.context.nativeControlUiEpoch = 7;
  fixture.context.nativeControlTakeoverPending = true;
  const handshake = fixture.context.writeControlIntent({}, false);
  fixture.requests[0].resolve({accepted: true, ui_control_epoch: 7});
  assert.equal((await handshake).sent, true);
  assert.equal(fixture.context.nativeControlUiEpoch, 7);
  assert.equal(fixture.context.nativeControlTakeoverPending, false);
  assert.equal(fixture.pressed.size, 0);
});

test('retired page stops ordinary refreshes while legal ESTOP retains its session and priority', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  useProductionIntentEnvelope(fixture);
  const obsolete = fixture.context.writeControlIntent({}, false);
  fixture.requests[0].resolve({accepted: false, reason: 'input_page_not_current'});
  assert.equal((await obsolete).sent, false);
  assert.equal(fixture.context.nativeControlPageRetired, true);
  assert.match(fixture.context.statusPanel.textContent, /当前阻挡.*下一步/);
  assert.equal((await fixture.context.writeControlIntent({}, false)).reason, 'input_page_not_current');
  assert.equal(fixture.requests.length, 1, 'retired page kept sending ordinary refreshes');
  fixture.context.estopLatched = true;
  const urgent = fixture.context.writeControlIntent({estop: true}, true);
  assert.equal(fixture.requests[1].intent.estop, true);
  assert.equal(fixture.requests[1].intent.session_id, 'test-session');
  fixture.requests[1].resolve({accepted: true, ui_control_epoch: 9});
  assert.equal((await urgent).sent, true);
  assert.equal(fixture.context.nativeControlPageRetired, true);
  assert.equal(fixture.context.nativeControlUiEpoch, 1, 'retired ESTOP silently adopted a new ordinary epoch');
});

test('session reset discards pending old ESTOP and allows a new session without waiting', async () => {
  const writes=[],aborts=[];
  const queue=logic.createLatestControlWriteQueue((extra,announce,token)=>new Promise(resolve=>writes.push({extra,resolve,token})),null,(token,reason)=>aborts.push({token,reason}));
  const first=queue.send({},false),second=queue.send({},false);
  const urgent=queue.send({estop:true},false);
  const oldPending=queue.send({estop:true},false);
  assert.equal(writes.length,3);queue.reset();
  assert.equal((await oldPending).reason,'control_queue_session_changed');
  assert.equal(aborts.filter(x=>x.reason==='session_changed').length,3);
  const next=queue.send({},false);assert.equal(writes.length,4);assert.equal(Boolean(writes[3].extra.estop),false);
  writes.slice(0,3).forEach(x=>x.resolve({sent:false}));await Promise.all([first,second,urgent]);
  writes[3].resolve({sent:true});await next;
  assert.equal(writes.length,4,'old completion replayed discarded input in new session');
});

test('out-of-order response cannot rearm, confirm old gear or revoke a newer input', async () => {
  const fixture = intentPageFixture(['ArrowUp']);
  const old = fixture.context.writeControlIntent({}, false);
  fixture.context.nativeIntentSeq++;
  const current = fixture.context.writeControlIntent({}, false);
  fixture.requests[1].resolve({accepted:true,intent_seq:63}); await current;
  fixture.requests[0].resolve({accepted:false,reason:'fresh_neutral_required'});
  assert.equal((await old).reason,'superseded_control_intent_response');
  assert.equal(fixture.pressed.has('ArrowUp'),true);
  const expiredAuth=fixture.context.writeControlIntent({},false);
  fixture.context.nativeIntentSeq++;
  fixture.requests[2].reject(Object.assign(new Error('old auth result'),{status:401}));
  assert.equal((await expiredAuth).reason,'superseded_control_intent_response');
  assert.equal(fixture.context.controlAuthorityLost,false);
});

test('ICE server error preserves transport and distinguishes DNS from timeout without logging raw text', () => {
  const dns=logic.iceServerError({url:'turn:user:secret@example.test:6000?transport=tcp',errorText:'TURN host lookup received error'});
  assert.equal(dns.server_transport,'tcp');assert.equal(dns.error_category,'dns_resolution');
  assert.equal(logic.iceServerError({url:'stun:example.test:3478',errorText:'request timed out'}).error_category,'server_timeout');
  assert.equal(logic.iceServerError({url:'turns:example.test:5349'}).server_transport,'tcp');
  assert.equal(logic.iceServerError({url:'turn:example.test:3478'}).server_transport,'udp');
  assert(!/secret|example/.test(JSON.stringify(dns)));
});

test('ICE diagnostics retain failed direct checks, bound output and omit addresses and credentials', () => {
  const report=new Map();
  report.set('host',{type:'local-candidate',candidateType:'host',protocol:'udp',address:'private-address'});
  report.set('srflx',{type:'remote-candidate',candidateType:'srflx',protocol:'udp',address:'public-address'});
  report.set('relay',{type:'local-candidate',candidateType:'relay',protocol:'udp',relayProtocol:'tcp',username:'secret'});
  for(let i=0;i<40;i++)report.set('pair'+i,{id:'pair'+i,type:'candidate-pair',state:'waiting',localCandidateId:'relay',remoteCandidateId:'srflx'});
  report.set('direct',{id:'direct',type:'candidate-pair',state:'failed',localCandidateId:'host',remoteCandidateId:'srflx',requestsSent:6,responsesReceived:0});
  const summary=logic.iceConnectivity(report,{id:'pair39'});
  assert.equal(summary.pairs.length,16);assert.equal(summary.pairs_truncated,true);
  assert.equal(summary.pairs[0].id,'pair39');assert.equal(summary.pairs[0].relay_protocol,'tcp');
  assert.equal(summary.pairs[1].id,'direct');assert.equal(summary.pairs[1].responses_received,0);
  assert.equal(summary.candidates.local['host/udp'],1);assert.equal(summary.pair_states.failed,1);
  assert(!/private-address|public-address|secret/.test(JSON.stringify(summary)));
});

test('critical video health logs transitions and bounded stale snapshots with local wait evidence', () => {
  const logs=[];let now=0;
  const context=vm.createContext({polling:true,nativeControlSessionId:'s',nativeControlSessionGeneration:1,
    performance:{now:()=>now},document:{hidden:false,visibilityState:'visible'},latestMetrics:{streams:[],sampled_at_ms:123},
    controlTraceSummary:{timer_lag_max_ms:210,long_task_max_ms:180},clientLog:(event,details)=>logs.push({event,details})});
  const a=controlPageSource.indexOf("let lastCriticalVideoHealthKey="),b=controlPageSource.indexOf('function driverCriticalVideoHealth()',a);
  vm.runInContext(controlPageSource.slice(a,b),context);
  const bad={ready:false,missing:['front'],cameras:[{camera_id:'front',reason:'track_muted'}]};
  context.recordCriticalVideoHealth(bad,500);
  for(now=10;now<1000;now+=10)context.recordCriticalVideoHealth(bad,500);
  assert.equal(logs.length,1);context.recordCriticalVideoHealth(bad,500);assert.equal(logs.length,2);
  context.recordCriticalVideoHealth({ready:true,missing:[],cameras:[{camera_id:'front',reason:'ready'}]},500);
  assert.equal(logs.length,3);assert.equal(logs[0].details.heartbeat_lag_max_ms,210);
  assert.equal(logs[0].details.long_task_max_ms,180);assert.equal(logs[0].details.document_hidden,false);
});

test('receiver latency counts jitter residence once and missing stats stay unknown', () => {
  const stats={framesDecoded:100,totalProcessingDelay:12,jitterBufferDelay:10,
    jitterBufferEmittedCount:100,totalDecodeTime:0.6};
  const measured=logic.receiverLatency(stats,20,0.1);
  assert.equal(measured.latencyMs,190); // 20 + 50 + 120, not 290.
  assert.equal(measured.jitterMs,100);assert.equal(measured.decodeMs,6);
  assert.equal(measured.method,'rtp-arrival-to-decode');
  delete stats.totalProcessingDelay;
  assert.equal(logic.receiverLatency(stats,20,0.1).latencyMs,176);
  assert.equal(logic.receiverLatency(stats,20,0.1).method,'jitter-buffer-plus-decode');
  for(const value of [undefined,null,NaN,Infinity,-1]){
    assert.equal(logic.receiverLatency({},value,0.1).latencyMs,null);
    assert.equal(logic.receiverLatency(stats,20,value).latencyMs,null);
  }
  assert.equal(logic.receiverLatency({framesDecoded:0,totalProcessingDelay:0},20,0.1).latencyMs,null);
  assert.equal(logic.receiverLatency({framesDecoded:1,totalProcessingDelay:0},0,0).latencyMs,0);
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
    recordCriticalVideoHealth:health=>health, cameraView:{criticalHealth:(ids,staleMs)=>({ready:ids.length>0,missing:ids,staleMs})}});
  const handler=controlPageSource.match(/^function driverCriticalVideoHealth\(\)\{[\s\S]*?^\}/m);
  assert(handler);vm.runInContext(handler[0],context);
  assert.equal(context.driverCriticalVideoHealth().ready,false);
  context.mediaStatus={lanes:[{camera_id:'aux',critical_for_control:false}]};
  assert.equal(context.driverCriticalVideoHealth().ready,true);
  context.mediaStatus={lanes:['front','left','rear1','rear2'].map(camera_id=>({camera_id,critical_for_control:false}))};
  assert.equal(context.driverCriticalVideoHealth().ready,true);
  // A single explicitly critical lane restores protection for a lost view.
  context.cameraView.criticalHealth=()=>({ready:false,missing:['front'],cameras:[]});
  context.mediaStatus.lanes[0].critical_for_control=true;
  assert.equal(context.driverCriticalVideoHealth().ready,false);
  context.cameraView.criticalHealth=(ids,staleMs)=>({ready:ids.length>0,missing:ids,staleMs});
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
  fixture.context.latestRuntimeStatus.time_sync_readiness={ready:false,latest_candidate_healthy:false};
  const request=fixture.context.writeControlIntent({},true);
  assert.equal(fixture.requests[0].intent.throttle,0);
  assert.equal(fixture.requests[0].intent.steering,0);
  assert.equal(fixture.context.controlProfileState.acknowledged,true);
  assert.equal(fixture.context.controlAuthorityLost,false);
  fixture.requests[0].resolve({accepted:true});await request;
  fixture.context.driverTimeSyncReady=()=>true;
  fixture.context.latestRuntimeStatus.time_sync_readiness={ready:true,latest_candidate_healthy:true};
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
  for (const [cause, label] of [['time_sync',/时间同步/], ['input_lease',/输入刷新/], ['control_link',/控制命令/]]) {
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
    performance:{now:()=>1000},renderMonitoring(){},updateVcuHandshakeState(value){context.vcuHandshake=value}});
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

// Run the shipped logout and VCU sender against a local deterministic transport.
// Completion evidence is a real vehicle-telemetry schema, not an HTTP success.
function logoutPageFixture() {
  let elapsed=1000,timerId=0,closeCount=0;
  const timers=new Map(),calls=[],packets=[];
  class Clock extends Date { static now(){return 100000+elapsed} }
  const runtime={authenticated:true,connected:true,driver_id:'driver',vehicle_id:'vehicle',session_id:'session',
    control_session_generation:7,connect_cancellation_generation:9,native_control:{ui_control_epoch:3,owner_ui_instance_id:'page'},time_sync:{synchronized:true,uncertainty_ms:1,offset_ms:0}};
  const context=vm.createContext({controlLogic:logic,safeLogoutAttempt:null,connectRequestGeneration:1,
    nativeControlSessionId:'session',nativeControlSessionGeneration:7,nativeControlUiEpoch:3,uiInstanceId:'page',
    lastControlStatusSeq:50,latestRuntimeStatus:{...runtime,time_sync:{synchronized:true,uncertainty_ms:1,offset_ms:0}},
    vehicleTelemetry:null,vehicleTelemetryReceivedAt:null,authenticated:true,polling:true,connecting:false,
    controlAuthorityLost:false,estopLatched:false,driverIdInput:{value:'driver'},
    peer:{connectionState:'connected'},controlChannel:{readyState:'open',send:data=>packets.push(JSON.parse(data))},
    vcuHandshake:{supported:true,state:'ready',ready:true,adapter_ready:true},controlProfileState:{acknowledged:true},
    performance:{now:()=>elapsed},Date:Clock,AbortController:OfflineAbortController,
    driverTimeSyncReady:()=>Boolean(context.latestRuntimeStatus.time_sync&&context.latestRuntimeStatus.time_sync.synchronized===true),
    connectButton:{},webrtcLabel:{},statusPanel:{},sessionPanel:{},vcuPanel:{},monitorPanel:{},loginPanel:{},
    clientLog:(event,details)=>calls.push({event,details}),resetControlAuthorityInput:()=>calls.push({event:'clear_input'}),
    renderAuthExpiry(){},renderMonitoring(){},renderCanFeedback(){},
    closeRealtimeSession(){closeCount++;context.controlChannel=null;context.peer=null;context.nativeControlSessionId='';context.nativeControlSessionGeneration=0;context.nativeControlUiEpoch=0},
    get:async path=>{calls.push({get:path});return {...runtime,native_control:{...runtime.native_control}}},
    post:async(path,body)=>{calls.push({post:path,body});return {session_id:body.expected_session_id}},
    setTimeout(callback,delay){const id=++timerId;timers.set(id,{callback,at:elapsed+delay});return id},
    clearTimeout:id=>timers.delete(id)});
  context.controlTransportCourier={negotiated:()=>false,epoch:()=>0,activePath:()=>"wss"};
  context.mediaOfferScope={id:'attempt'};
  const managementStart=controlPageSource.indexOf('function controlManagementScope('),managementEnd=controlPageSource.indexOf('function currentMediaPeer(',managementStart);
  assert(managementStart>=0&&managementEnd>managementStart);vm.runInContext(controlPageSource.slice(managementStart,managementEnd),context);
  const sender=controlPageSource.split('\n').find(line=>line.startsWith('function sendVcuHandshakeCommand('));
  assert(sender);vm.runInContext(sender,context);
  const start=controlPageSource.indexOf('function pageReleaseScope('),end=controlPageSource.indexOf("addEventListener('pagehide'",start);
  assert(start>=0&&end>start);vm.runInContext(controlPageSource.slice(start,end),context);
  async function settle(){await new Promise(resolve=>setImmediate(resolve))}
  async function advanceTo(now){
    for(;;){const due=[...timers.entries()].filter(([,timer])=>timer.at<=now).sort((a,b)=>a[1].at-b[1].at)[0];if(!due)break;
      elapsed=due[1].at;timers.delete(due[0]);due[1].callback();await settle()}
    elapsed=now;await settle();
  }
  function telemetry(overrides={}){
    const message={event:'vehicle_telemetry',session_id:'session',vehicle_id:'vehicle',driver_id:'driver',control_status_seq:51,sent_at_utc_ms:Clock.now(),estop:false,
      vehicle_adapter:{opened:true,healthy:true},vcu_handshake:{supported:true,state:'disarmed',requested:false,ready:false,disarming:false},
      can_feedback:{supported:true,feedback_fresh:true,max_feedback_age_ms:5,speed_valid:true,speed_mps:0,gear_valid:true,gear:1,
        handshake_valid:true,handshake_status:3,parking_brake_valid:[true,true,true,true],parking_brake_status:[2,2,2,2]},...overrides};
    context.vehicleTelemetry=message;context.vehicleTelemetryReceivedAt=elapsed;return message;
  }
  return{context,runtime,calls,packets,settle,advanceTo,telemetry,closeCount:()=>closeCount,now:()=>elapsed};
}

test('product VCU management stays on authenticated cloud after route negotiation with an open DC',async()=>{
  const fixture=logoutPageFixture();fixture.context.controlTransportCourier={negotiated:()=>true};
  await fixture.context.sendVcuHandshakeCommand('disconnect');
  assert.equal(fixture.packets.length,0);
  const sent=fixture.calls.find(call=>call.post==='/api/control-transport/management');
  assert(sent);assert.equal(sent.body.source_session_id,'session');assert.equal(sent.body.ui_control_epoch,3);
  assert.equal(sent.body.message.action,'disconnect');
});

test('shipped logout waits for a new final CAN disarmed report before releasing the session and preserves ESTOP', async()=>{
  const fixture=logoutPageFixture();fixture.context.estopLatched=true;
  const exit=fixture.context.logout();await fixture.settle();
  assert.equal(fixture.packets.length,1);assert.equal(fixture.packets[0].event,'vcu_handshake_command');assert.equal(fixture.packets[0].action,'disconnect');
  assert.equal(fixture.closeCount(),0);assert(!fixture.calls.some(call=>call.post));
  fixture.telemetry({vcu_handshake:{supported:true,state:'disarm_manual',requested:false,ready:false,disarming:true}});
  await fixture.advanceTo(1100);assert.equal(fixture.closeCount(),0);assert(!fixture.calls.some(call=>call.post));
  fixture.telemetry({control_status_seq:52,estop:true});await fixture.advanceTo(1200);
  const result=await exit;assert.equal(result.confirmed,true);assert.equal(fixture.closeCount(),1);assert.equal(fixture.context.authenticated,false);
  const release=fixture.calls.find(call=>call.post==='/api/disconnect');assert.equal(release.body.expected_session_id,'session');
  assert.equal(release.body.expected_control_session_generation,7);assert.equal(release.body.expected_ui_control_epoch,3);assert.equal(release.body.ui_instance_id,'page');
  assert.equal(fixture.context.estopLatched,true);assert.match(fixture.context.statusPanel.textContent,/最终 CAN 写入已确认/);
  assert.match(fixture.context.statusPanel.textContent,/须本地确认后复位/);assert(!fixture.context.statusPanel.textContent.includes('已安全退出'));
});

test('shipped logout bounds confirmation at 15 seconds and HTTP release alone never proves safety', async()=>{
  const fixture=logoutPageFixture();const exit=fixture.context.logout();await fixture.settle();
  await fixture.advanceTo(15900);assert(!fixture.calls.some(call=>call.post));
  await fixture.advanceTo(16000);const result=await exit;assert.equal(result.confirmed,false);assert.equal(result.reason,'timeout');
  assert.equal(fixture.closeCount(),1);assert.match(fixture.context.statusPanel.textContent,/安全退出未确认.*超时/);
  assert.match(fixture.context.statusPanel.textContent,/本地核实停车和制动状态/);
});

test('negotiated WSS logout awaits management delivery and fresh final CAN evidence after DC closes', async()=>{
  const fixture=logoutPageFixture();let deliver;const messages=[];
  fixture.context.controlChannel=null;
  fixture.context.controlTransportCourier={negotiated:()=>true,activePath:()=> 'wss',epoch:()=>2};
  fixture.context.sendControlManagement=message=>{messages.push(message);return new Promise(resolve=>{deliver=resolve})};
  const exit=fixture.context.logout();await fixture.settle();
  assert.equal(messages.length,1);assert.equal(messages[0].action,'disconnect');assert.equal(fixture.closeCount(),0);
  assert(!fixture.calls.some(call=>call.post));deliver({});await fixture.settle();
  assert(!fixture.calls.some(call=>call.post));fixture.telemetry();await fixture.advanceTo(1100);
  assert.equal((await exit).confirmed,true);assert.equal(fixture.closeCount(),1);
});

test('negotiated WSS logout cannot confirm across a transport epoch change', async()=>{
  const fixture=logoutPageFixture();let epoch=2;
  fixture.context.controlChannel=null;
  fixture.context.controlTransportCourier={negotiated:()=>true,activePath:()=> 'wss',epoch:()=>epoch};
  fixture.context.sendControlManagement=async()=>({});
  const exit=fixture.context.logout();await fixture.settle();epoch=3;
  fixture.telemetry();await fixture.advanceTo(1100);
  assert.equal((await exit).confirmed,false);assert.match(fixture.context.statusPanel.textContent,/安全退出未确认/);
});

test('shipped logout rejects old sequence, old source feedback and untrusted clock then accepts only fresh current evidence', async()=>{
  const fixture=logoutPageFixture();const exit=fixture.context.logout();await fixture.settle();
  fixture.telemetry({control_status_seq:50});await fixture.advanceTo(1100);assert(!fixture.calls.some(call=>call.post));
  fixture.telemetry({control_status_seq:51,sent_at_utc_ms:100900});await fixture.advanceTo(1200);assert(!fixture.calls.some(call=>call.post));
  fixture.context.latestRuntimeStatus.time_sync.synchronized=false;fixture.telemetry({control_status_seq:52});
  await fixture.advanceTo(1300);assert(!fixture.calls.some(call=>call.post));
  fixture.context.latestRuntimeStatus.time_sync.synchronized=true;fixture.telemetry({control_status_seq:53});
  await fixture.advanceTo(1400);assert.equal((await exit).confirmed,true);
});

test('shipped logout leaves a replacement session and peer untouched', async()=>{
  const fixture=logoutPageFixture();const exit=fixture.context.logout();await fixture.settle();
  fixture.context.nativeControlSessionId='replacement';fixture.context.nativeControlSessionGeneration=8;
  const replacement={connectionState:'connected'};fixture.context.peer=replacement;
  fixture.telemetry({session_id:'replacement',control_status_seq:52});await fixture.advanceTo(1100);
  assert.equal((await exit).reason,'session_changed');assert.equal(fixture.closeCount(),0);assert(!fixture.calls.some(call=>call.post));
  assert.equal(fixture.context.peer,replacement);assert.equal(fixture.context.authenticated,true);assert.match(fixture.context.statusPanel.textContent,/安全退出未确认/);
});

test('shipped logout on a retired page cannot disconnect the owner even before VCU command', async()=>{
  const fixture=logoutPageFixture();fixture.runtime.native_control={ui_control_epoch:4,owner_ui_instance_id:'replacement-page'};
  assert.equal((await fixture.context.logout()).reason,'session_changed');assert.equal(fixture.packets.length,0);
  assert.equal(fixture.closeCount(),0);assert(!fixture.calls.some(call=>call.post));
});

test('shipped logout reports unsupported adapters and link loss as unconfirmed', async()=>{
  const unsupported=logoutPageFixture();unsupported.context.vcuHandshake={supported:false,state:'unsupported'};
  assert.equal((await unsupported.context.logout()).reason,'unsupported');assert.equal(unsupported.packets.length,0);
  assert.match(unsupported.context.statusPanel.textContent,/控制会话已释放；安全退出未确认/);
  const lost=logoutPageFixture();const exit=lost.context.logout();await lost.settle();lost.context.controlChannel.readyState='closed';
  await lost.advanceTo(1100);assert.equal((await exit).reason,'link_lost');assert.match(lost.context.statusPanel.textContent,/反馈链路中断/);
});

test('shipped logout does not treat failed HTTP release as safety completion and can retry', async()=>{
  const fixture=logoutPageFixture();fixture.context.post=async()=>{throw Error('local release rejected')};
  const exit=fixture.context.logout();await fixture.settle();fixture.telemetry();await fixture.advanceTo(1100);
  assert.equal((await exit).reason,'release_failed');assert.equal(fixture.closeCount(),0);assert.equal(fixture.context.authenticated,true);
  assert.match(fixture.context.statusPanel.textContent,/安全退出未确认/);assert.equal(fixture.context.safeLogoutAttempt,null);
  fixture.context.post=async()=>({session_id:'session'});const retry=fixture.context.logout();await fixture.settle();
  fixture.telemetry({control_status_seq:52});await fixture.advanceTo(1200);assert.equal((await retry).confirmed,true);
});

test('logout feedback helper requires complete new CAN evidence and never substitutes a handshake HTTP acknowledgement',()=>{
  const fixture=logoutPageFixture();const request={session_id:'session',vehicle_id:'vehicle',driver_id:'driver',status_floor:50,started_at_monotonic_ms:1000,source_floor_utc_ms:101000};
  const message=fixture.telemetry(),judge=value=>logic.safeLogoutFeedback(request,value,1000,1000,101000);
  assert.equal(judge(message).confirmed,true);
  for(const changed of [{event:'vcu_handshake_status'},{session_id:'old'},{driver_id:'other-driver'},{sent_at_utc_ms:101001},{sent_at_utc_ms:100999},
    {can_feedback:{...message.can_feedback,parking_brake_valid:[true,true,false,true]}},
    {can_feedback:{...message.can_feedback,feedback_fresh:false}},
    {can_feedback:{...message.can_feedback,gear:3}}, {can_feedback:{...message.can_feedback,speed_mps:0.11}},
    {vehicle_adapter:{opened:true,healthy:false}}, {vcu_handshake:{...message.vcu_handshake,disarming:true}}]){
    assert.equal(judge({...message,...changed}).confirmed,false,JSON.stringify(changed));
  }
  assert.equal(logic.safeLogoutFeedback(request,message,1000,1601,101601).confirmed,false);
  assert.equal(logic.safeLogoutFeedback(request,message,1000,1000,null).confirmed,false);
});

test('ordinary writes are frozen during logout while a legal ESTOP remains dispatchable',async()=>{
  const fixture=intentPageFixture();fixture.context.safeLogoutAttempt={};
  assert.equal((await fixture.context.writeControlIntent({},true)).reason,'safe_logout_in_progress');assert.equal(fixture.requests.length,0);
  fixture.context.estopLatched=true; // The shipped emergency button latches before send.
  const urgent=fixture.context.writeControlIntent({estop:true},true);assert.equal(fixture.requests.length,1);assert.equal(fixture.requests[0].intent.estop,true);
  fixture.requests[0].resolve({accepted:true});assert.equal((await urgent).sent,true);
});

test('shipped logout sends one VCU exit for repeated clicks and turns a failed VCU send into an unconfirmed release',async()=>{
  const repeated=logoutPageFixture();const first=repeated.context.logout(),second=repeated.context.logout();await repeated.settle();
  assert.equal(repeated.packets.length,1);repeated.telemetry();await repeated.advanceTo(1100);await Promise.all([first,second]);
  assert.equal(repeated.calls.filter(call=>call.post==='/api/disconnect').length,1);
  const failed=logoutPageFixture();failed.context.controlChannel.send=()=>{throw Error('DataChannel send failed')};
  assert.equal((await failed.context.logout()).reason,'request_failed');assert.equal(failed.closeCount(),1);
  assert(failed.calls.some(call=>call.post==='/api/disconnect'));assert.match(failed.context.statusPanel.textContent,/安全退出未确认/);
});

test('shipped logout excludes disarmed feedback received during the HTTP preflight before the real VCU request',async()=>{
  const fixture=logoutPageFixture();let resolvePreflight,reads=0;
  fixture.context.get=async()=>++reads===1?await new Promise(resolve=>{resolvePreflight=resolve}):fixture.runtime;
  const exit=fixture.context.logout();await fixture.settle();fixture.telemetry();fixture.context.lastControlStatusSeq=51;
  await fixture.advanceTo(1100);resolvePreflight(fixture.runtime);await fixture.settle();
  assert.equal(fixture.packets.length,1);await fixture.advanceTo(1200);assert(!fixture.calls.some(call=>call.post));
  fixture.telemetry({control_status_seq:52});await fixture.advanceTo(1300);assert.equal((await exit).confirmed,true);
});

test('shipped logout without an active vehicle releases only the scoped login and does not invent VCU completion',async()=>{
  const fixture=logoutPageFixture();Object.assign(fixture.context,{nativeControlSessionId:'',nativeControlSessionGeneration:0,nativeControlUiEpoch:0});
  Object.assign(fixture.runtime,{session_id:'',connected:false,native_control:{ui_control_epoch:0,owner_ui_instance_id:''}});
  assert.equal((await fixture.context.logout()).reason,'no_active_session');assert.equal(fixture.packets.length,0);
  const release=fixture.calls.find(call=>call.post==='/api/disconnect');assert.equal(release.body.expected_session_id,'');
  assert.equal(release.body.expected_control_session_generation,7);assert.equal(release.body.expected_driver_id,'driver');
  assert.match(fixture.context.statusPanel.textContent,/安全退出未确认.*没有活动车端会话/);
});

test('shipped logout preserves a new local peer established while the scoped HTTP release is in flight',async()=>{
  const fixture=logoutPageFixture();let completeRelease;
  fixture.context.post=async()=>await new Promise(resolve=>{completeRelease=resolve});
  const exit=fixture.context.logout();await fixture.settle();fixture.telemetry();await fixture.advanceTo(1100);
  assert(completeRelease);const replacement={connectionState:'connected'};
  fixture.context.peer=replacement;fixture.context.nativeControlSessionId='replacement';fixture.context.nativeControlSessionGeneration=8;
  completeRelease({session_id:'session'});await fixture.settle();assert.equal((await exit).reason,'session_changed');
  assert.equal(fixture.closeCount(),0);assert.equal(fixture.context.peer,replacement);assert.equal(fixture.context.authenticated,true);
});

test('shipped logout consumes a missing HTTP release response with a bounded unconfirmed result and keeps ESTOP',async()=>{
  const fixture=logoutPageFixture();fixture.context.estopLatched=true;fixture.context.post=()=>new Promise(()=>{});
  const exit=fixture.context.logout();await fixture.settle();fixture.telemetry();await fixture.advanceTo(1100);
  await fixture.advanceTo(2600);assert.equal((await exit).reason,'release_failed');assert.equal(fixture.closeCount(),0);
  assert.equal(fixture.context.estopLatched,true);assert.equal(fixture.context.safeLogoutAttempt,null);assert.match(fixture.context.statusPanel.textContent,/安全退出未确认/);
});

test('shipped pagehide preserves the old release scope before clearing local session fields',()=>{
  const fixture=logoutPageFixture(),listeners=new Map(),requests=[];
  fixture.context.addEventListener=(event,handler)=>listeners.set(event,handler);fixture.context.flushControlTrace=()=>{};
  fixture.context.fetch=(url,options)=>{requests.push({url,options});return Promise.resolve({})};
  const declaration=controlPageSource.split('\n').find(line=>line.startsWith("addEventListener('pagehide'"));assert(declaration);
  vm.runInContext(declaration,fixture.context);listeners.get('pagehide')();
  assert.equal(fixture.context.nativeControlSessionId,'');assert.equal(fixture.closeCount(),1);
  const body=JSON.parse(requests[0].options.body);assert.equal(body.expected_session_id,'session');assert.equal(body.expected_control_session_generation,7);
  assert.equal(body.expected_ui_control_epoch,3);assert.equal(body.expected_connect_cancellation_generation,9);assert.equal(requests[0].options.keepalive,true);
});

test('shipped signaling failure releases only its snapshotted scope after closing local transport',async()=>{
  const fixture=logoutPageFixture();fixture.context.signalingGeneration=1;fixture.context.signalingPollAbort=null;
  fixture.context.post=async(path,body)=>{if(path==='/api/poll-signaling')throw Error('signal transport lost');fixture.calls.push({post:path,body});return {}};
  const declaration=controlPageSource.split('\n').find(line=>line.startsWith('async function pollSignaling('));assert(declaration);
  vm.runInContext(declaration,fixture.context);await fixture.context.pollSignaling(1);
  const release=fixture.calls.find(call=>call.post==='/api/end-session');assert(release);
  assert.equal(fixture.context.nativeControlSessionId,'');assert.equal(release.body.expected_session_id,'session');
  assert.equal(release.body.expected_control_session_generation,7);assert.equal(release.body.expected_ui_control_epoch,3);
});

test('shipped connect setup failure scopes cleanup to its returned session before clearing local metadata',async()=>{
  const fixture=logoutPageFixture();Object.assign(fixture.context,{nativeControlSessionId:'',nativeControlSessionGeneration:0,nativeControlUiEpoch:0,
    polling:false,vehicleSelect:{value:'vehicle'},signalingGeneration:1,controlTraceScope:{},lastNativeIntentSnapshot:'',
    nativeControlTakeoverPending:false,nativeControlPageRetired:false,flushControlTrace(){},setControlTraceScope(){}});
  Object.assign(fixture.runtime,{connected:false,session_id:''});Object.assign(fixture.context.latestRuntimeStatus,{connected:false,session_id:''});
  const connected={driver_id:'driver',vehicle_id:'vehicle',session_id:'new-session',control_session_generation:8,ui_control_epoch:0,connect_cancellation_generation:10};
  fixture.context.post=async(path,body)=>{
    fixture.calls.push({post:path,body});if(path==='/api/connect')return connected;
    if(path==='/api/webrtc/ice-servers')throw Error('media setup failed');return {};
  };
  const declaration=controlPageSource.split('\n').find(line=>line.startsWith('async function connect('));assert(declaration);
  vm.runInContext(declaration,fixture.context);await assert.rejects(fixture.context.connect(),/media setup failed/);
  const release=fixture.calls.find(call=>call.post==='/api/end-session');assert(release);
  assert.equal(release.body.expected_session_id,'new-session');assert.equal(release.body.expected_control_session_generation,8);
  assert.equal(release.body.expected_ui_control_epoch,0);assert.equal(release.body.expected_connect_cancellation_generation,10);
  assert.equal(fixture.calls.find(call=>call.post==='/api/connect').body.ui_instance_id,'page');
});

test('shipped logout of an unclaimed session allows only its recorded creator page',async()=>{
  for(const creator of ['page','other-page',undefined]){
    const fixture=logoutPageFixture();fixture.context.nativeControlUiEpoch=0;fixture.context.vcuHandshake={supported:false,state:'unsupported'};
    fixture.runtime.native_control={ui_control_epoch:0,owner_ui_instance_id:''};fixture.runtime.session_creator_ui_instance_id=creator;
    const result=await fixture.context.logout();assert.equal(result.reason,creator==='page'?'unsupported':'session_changed');
    assert.equal(fixture.calls.some(call=>call.post==='/api/disconnect'),creator==='page');
  }
});

function recordingLane(overrides={}) {
  return {schema_version:1,configured_enabled:true,active:true,state:'recording',incomplete:false,
    issue_code:'',next_action:'',recording_attempt_id:'recording-attempt',status_revision:2,
    accepted_access_units:12,worker_input_access_units:12,finalized_segments:0,unfinished_segments:1,
    final_eos_received:false,cleanup_pending:false,summary_persisted:true,summary_revision:2,reported_at_utc_ms:100166,...overrides};
}

async function recordingMonitorFixture() {
  const fixture=intentPageFixture(['ArrowUp']);await fixture.advanceTo(0);
  const camera=useProductionCameraView(fixture);useProductionIntentEnvelope(fixture);
  for(const now of [100,133,166]){await fixture.advanceTo(now);camera.video.frames++;fixture.context.driverCriticalVideoHealth()}
  const metrics=new Map(),context=fixture.context;
  class Element {
    constructor(){this.children=[];this.textContent='';this.className='';this.classList={remove(){},add(){}}}
    appendChild(child){this.children.push(child)}
    replaceChildren(){this.children=[]}
  }
  const document={createElement:()=>new Element(),getElementById:id=>{if(!metrics.has(id))metrics.set(id,new Element());return metrics.get(id)}};
  class Clock extends Date {static now(){return 100000+context.performance.now()}}
  Object.assign(context,{document,Date:Clock,authenticated:true,mediaStatus:{lanes:[]},mediaStatusReceivedAt:null,
    recordingMonitorState:{scopeKey:'',incomplete:false},latestMetrics:{codec:'h264',backend:'software',streams:[{camera_id:'front',fps:30,bitrate_kbps:2000,packet_loss_percent:0,estimated_end_to_end_latency_ms:100}]},
    monitorPanel:new Element(),alertsPanel:new Element(),streamMetrics:new Element(),vehicleSelect:{value:'vehicle'},lastAlertKey:'',
    renderEstopRequest(){},renderControlRecovery:()=>({visible:false}),renderVcuHandshake(){},renderCanFeedback(){},renderControlState(){},
    hasTurnServer:()=>false,formatMetric:(value,digits,suffix)=>value==null?'未知':Number(value).toFixed(digits)+(suffix||''),
    updateVcuHandshakeState(){throw Error('recording-only status changed the VCU gate')}});
  Object.assign(context.latestRuntimeStatus,{vehicle_id:'vehicle',connected:true,session_id:'test-session',
    authorized_vehicles:[{vehicle_id:'vehicle',online:true}],time_sync:{synchronized:true,uncertainty_ms:1,offset_ms:0}});
  context.consoleConfig.max_time_sync_uncertainty_ms=25;
  const render=controlPageSource.split('\n').find(line=>line.startsWith('function renderMonitoring('));assert(render);
  const metric=controlPageSource.split('\n').find(line=>line.startsWith('function setMetric('));assert(metric);
  const a=controlPageSource.indexOf('function renderRecordingStatus('),b=controlPageSource.indexOf('function driverTimeSyncReady(',a);
  const apply=controlPageSource.match(/^function applyMediaStatus\(payload\)\{[\s\S]*?^\}/m);assert(a>=0&&b>a&&apply);
  vm.runInContext(metric+'\n'+controlPageSource.slice(a,b)+'\n'+render+'\n'+apply[0],context);
  const publish=(recording,extra={})=>context.applyMediaStatus({vehicle_id:'vehicle',session_id:'test-session',reported_at_utc_ms:Clock.now(),
    control_issue_code:'',lanes:[{camera_id:'front',critical_for_control:true,recording}],...extra});
  return {fixture,context,metrics,publish,camera};
}

test('actual shipped monitor reports recording failures and next action without changing held input or video readiness',async()=>{
  const {fixture,context,metrics,publish}=await recordingMonitorFixture();
  const input=JSON.stringify(context.currentControl()),profile=JSON.stringify(context.controlProfileState);
  const intent=JSON.stringify(context.nativeIntentEnvelope(context.currentControl()));assert.equal(context.driverCriticalVideoHealth().ready,true);
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_disk_full',next_action:'车辆空闲后清理磁盘空间，再检查录制日志'}));
  assert.equal(metrics.get('metric-recording').textContent,'录像不完整');assert.match(context.alertsPanel.textContent,/front.*录像不完整.*下一步.*清理磁盘/);
  assert.equal(context.alertsPanel.className,'alerts warn');assert.equal(context.driverCriticalVideoHealth().ready,true);
  assert.equal(JSON.stringify(context.currentControl()),input);assert.equal(JSON.stringify(context.controlProfileState),profile);
  assert.equal(JSON.stringify(context.nativeIntentEnvelope(context.currentControl())),intent);assert(fixture.pressed.has('ArrowUp'));assert.equal(fixture.requests.length,0);
  publish(recordingLane());assert.equal(metrics.get('metric-recording').textContent,'录像不完整');assert.match(context.alertsPanel.textContent,/不能补回此前缺口/);
});

test('actual shipped monitor keeps missing, mismatched and expired recording status unconfirmed',async()=>{
  const {context,metrics,publish}=await recordingMonitorFixture();
  publish(undefined);assert.match(metrics.get('metric-recording').textContent,/未确认/);
  publish(recordingLane(),{session_id:'old-session'});assert.match(metrics.get('metric-recording').textContent,/未确认/);
  publish(recordingLane(),{reported_at_utc_ms:0});assert.match(metrics.get('metric-recording').textContent,/未确认/);
  publish(recordingLane());assert.match(metrics.get('metric-recording').textContent,/录制进行中.*待结束确认/);
  context.mediaStatusReceivedAt-=3001;context.renderMonitoring();assert.match(metrics.get('metric-recording').textContent,/未确认/);
});

test('actual shipped monitor cannot refresh stale worker evidence with a new parent poll',async()=>{
  const {context,metrics,publish}=await recordingMonitorFixture();
  for(const workerTime of [undefined,null,0,97165,100167,'100166']){
    publish(recordingLane({reported_at_utc_ms:workerTime}));
    assert.match(metrics.get('metric-recording').textContent,/未确认/);
    assert.equal(context.driverCriticalVideoHealth().ready,true);assert(context.currentControl().throttle>0);
  }
  publish(recordingLane({reported_at_utc_ms:97166}));assert.match(metrics.get('metric-recording').textContent,/录制进行中/);
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_admission_overflow',reported_at_utc_ms:97165}));
  assert.match(metrics.get('metric-recording').textContent,/录像不完整.*未确认/);
  publish(recordingLane());assert.equal(metrics.get('metric-recording').textContent,'录像不完整');
});

test('actual shipped monitor accepts only real final EOS and closed segment evidence for current-attempt finalization',async()=>{
  const {context,metrics,publish}=await recordingMonitorFixture();
  const finalized=recordingLane({active:false,state:'finalized',final_eos_received:true,finalized_segments:1,unfinished_segments:0});
  for(const bad of [{final_eos_received:false},{unfinished_segments:1},{finalized_segments:0},{worker_input_access_units:11},{summary_persisted:false},{status_revision:0}]){
    publish({...finalized,...bad});assert.match(metrics.get('metric-recording').textContent,/未确认/);assert.doesNotMatch(metrics.get('metric-recording').textContent,/封装完成/);
  }
  publish(finalized);assert.match(metrics.get('metric-recording').textContent,/本次录像封装完成/);
  publish({...finalized,incomplete:true});assert.match(metrics.get('metric-recording').textContent,/完整性待确认/);
  publish(finalized);assert.match(metrics.get('metric-recording').textContent,/完整性待确认/);
  context.nativeControlSessionId='new-session';publish(finalized,{session_id:'new-session'});assert.match(metrics.get('metric-recording').textContent,/本次录像封装完成/);
});

test('actual shipped monitor handles malformed and duplicate recording lanes without throwing or claiming readiness',async()=>{
  const {context,metrics,publish}=await recordingMonitorFixture();
  for(const lanes of [[null],[false],[3],['invalid'],[{camera_id:'front',recording:3}],
    [{camera_id:'front',recording:recordingLane()},{camera_id:'front',recording:recordingLane()}],
    [{camera_id:'front',recording:recordingLane({accepted_access_units:null})}],
    [{camera_id:'front',recording:recordingLane({incomplete:'false'})}],
    [{camera_id:null,recording:recordingLane({incomplete:true,issue_code:'recording_admission_overflow'})}],
    [{camera_id:'front',recording:recordingLane({configured_enabled:false,active:true,state:'disabled'})}],
    [{camera_id:'front',recording:recordingLane({configured_enabled:false,active:false,state:'recording'})}]]){
    assert.doesNotThrow(()=>publish(undefined,{lanes}));assert.match(metrics.get('metric-recording').textContent,/未确认/);
    assert.equal(context.driverCriticalVideoHealth().ready,true);
  }
  context.latestRuntimeStatus.vehicle_id=undefined;publish(recordingLane(),{vehicle_id:undefined});
  assert.match(metrics.get('metric-recording').textContent,/未确认/);
});

test('actual shipped monitor distinguishes unconfirmed finalization evidence from proven AU loss and scopes both to the current session',async()=>{
  const {context,metrics,publish,fixture}=await recordingMonitorFixture();
  const input=JSON.stringify(context.currentControl());
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_status_unconfirmed',
    next_action:'保留片段并核查最终封装证据'}));
  assert.match(metrics.get('metric-recording').textContent,/证据不足.*完整性待确认/);
  assert.match(context.alertsPanel.textContent,/保留片段.*封装证据/);assert.doesNotMatch(context.alertsPanel.textContent,/缺口|缺录|无法补录|不能补回/);
  publish(recordingLane({active:false,state:'finalized',final_eos_received:true,finalized_segments:1,unfinished_segments:0}));
  assert.match(metrics.get('metric-recording').textContent,/完整性待确认/);assert.doesNotMatch(context.alertsPanel.textContent,/缺口|不能补回/);
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_admission_overflow'}));
  assert.equal(metrics.get('metric-recording').textContent,'录像不完整');assert.match(context.alertsPanel.textContent,/已确认缺口.*不能补回/);
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_status_unconfirmed'}));
  assert.equal(metrics.get('metric-recording').textContent,'录像不完整');assert.match(context.alertsPanel.textContent,/已确认缺口/);
  context.nativeControlSessionId='new-session';publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_status_unconfirmed'}),{session_id:'new-session'});
  assert.match(metrics.get('metric-recording').textContent,/完整性待确认/);assert.doesNotMatch(context.alertsPanel.textContent,/已确认缺口|不能补回/);
  publish(recordingLane(),{session_id:'new-session'});assert.match(metrics.get('metric-recording').textContent,/完整性待确认/);
  context.nativeControlSessionId='third-session';publish(recordingLane(),{session_id:'third-session'});
  assert.match(metrics.get('metric-recording').textContent,/录制进行中/);assert.equal(context.recordingMonitorState.incomplete,false);
  assert.equal(JSON.stringify(context.currentControl()),input);assert.equal(context.driverCriticalVideoHealth().ready,true);assert.equal(fixture.requests.length,0);
});

test('actual shipped monitor renders vehicle-only ESTOP and its source in the critical banner before recovery and recording warnings',async()=>{
  const {context,publish}=await recordingMonitorFixture();
  context.estopStatus={hidden:true,textContent:''};context.controlRecoveryStatus={hidden:true,textContent:''};
  context.vehicleTelemetry=null;context.vehicleTelemetryReceivedAt=null;context.polling=true;
  const banner=controlPageSource.split('\n').find(line=>line.startsWith('function renderEstopRequest('));
  const recovery=controlPageSource.split('\n').find(line=>line.startsWith('function renderControlRecovery('));
  assert(banner);assert(recovery);vm.runInContext(banner+'\n'+recovery,context);
  publish(recordingLane({state:'failed',active:false,incomplete:true,issue_code:'recording_admission_overflow',next_action:'检查录像存储'}));
  context.estopLatched=false;
  for(const [source,reason,label] of [['physical_estop','physical-test-reason',/物理急停/],
    ['page_request','page-test-reason',/控制页面/],['software_fault','software-test-reason',/软件故障/],['unknown_source','',/车端遥测.*急停/]]){
    context.vehicleTelemetry={estop:true,stop_source:source,stop_reason:reason,safety_state:'ESTOP',
      sent_at_utc_ms:context.Date.now(),control_timing:{control_braking_wait:true,control_recovery_cause:'critical_video'}};
    context.vehicleTelemetryReceivedAt=context.performance.now();context.renderMonitoring();
    assert.equal(context.estopLatched,false);assert.equal(context.estopStatus.hidden,false);assert.match(context.estopStatus.textContent,label);
    assert.equal(context.alertsPanel.className,'alerts critical');assert.match(context.alertsPanel.textContent,label);
    assert.match(context.alertsPanel.textContent,/录像不完整/);assert.equal(context.controlRecoveryStatus.hidden,true);
    if(reason){assert(context.estopStatus.textContent.includes(reason));assert(context.alertsPanel.textContent.includes(reason))}
    const shown=context.estopStatus.textContent;context.renderEstopRequest();assert.equal(context.estopStatus.textContent,shown);
  }
  context.estopLatched=true;context.vehicleTelemetry={estop:true,stop_source:'physical_estop',stop_reason:'still-physical'};
  context.renderMonitoring();assert.match(context.estopStatus.textContent,/物理急停.*still-physical/);
  context.vehicleTelemetry=null;context.renderMonitoring();assert.match(context.estopStatus.textContent,/请求已锁定.*等待车端/);
  assert.equal(context.alertsPanel.className,'alerts critical');
  context.estopLatched=false;context.renderMonitoring();assert.equal(context.estopStatus.hidden,true);
  assert.equal(context.alertsPanel.className,'alerts warn');assert.match(context.alertsPanel.textContent,/录像不完整/);
});

test('offline AbortController delivers one abort event and preserves its reason without disabling production cancellation',()=>{
  const controller=new OfflineAbortController(),reason=Object.assign(Error('deadline'),{name:'AbortError'}),events=[];
  controller.signal.addEventListener('abort',event=>events.push(event));controller.abort(reason);controller.abort(Error('second'));
  assert.equal(controller.signal.aborted,true);assert.equal(controller.signal.reason,reason);assert.equal(events.length,1);assert.equal(events[0].target,controller.signal);
});

// An unresolved Promise alone does not keep Node alive. A stuck asynchronous
// safety test must fail, rather than let the process exit with a false green.
const completionDeadline = setTimeout(() => {
  console.error(`Unfinished control logic tests: ${Array.from(unfinishedTests).join(', ')}`);
  process.exitCode = 1;
}, 10000);
Promise.all(pendingTests).then(() => {
  clearTimeout(completionDeadline);
  if (failed > 0) {
    console.error(`${failed} of ${passed + failed} control logic tests failed`);
    process.exitCode = 1;
  } else {
    console.log(`${passed} control logic tests passed`);
  }
});


test('ICE path classification never infers TURN UDP from candidate protocol or missing remote data', () => {
  const report=new Map([['l',{candidateType:'host',protocol:'udp'}],['r',{candidateType:'srflx',protocol:'udp'}]]);
  const pair={localCandidateId:'l',remoteCandidateId:'r'};
  assert.equal(logic.iceNetworkPath(report,pair),'p2p');
  report.get('l').candidateType='relay';assert.equal(logic.iceNetworkPath(report,pair),'turn_unknown');
  report.get('l').relayProtocol='udp';assert.equal(logic.iceNetworkPath(report,pair),'turn_udp');
  report.get('r').candidateType='relay';assert.equal(logic.iceNetworkPath(report,pair),'turn_unknown');
  report.get('r').relayProtocol='tcp';assert.equal(logic.iceNetworkPath(report,pair),'turn_tcp');
  report.get('r').relayProtocol='tls';assert.equal(logic.iceNetworkPath(report,pair),'turn_tls');
  report.delete('r');assert.equal(logic.iceNetworkPath(report,pair),'unknown');
});

test('ICE check summary covers truncated pairs and distinguishes absent counters from zero responses', () => {
  const report=new Map([['l',{type:'local-candidate',candidateType:'host',protocol:'udp'}],
    ['r',{type:'remote-candidate',candidateType:'srflx',protocol:'udp'}]]);
  for(let n=0;n<30;n++)report.set('p'+n,{id:'p'+n,type:'candidate-pair',state:'waiting',localCandidateId:'l',remoteCandidateId:'r'});
  report.set('failed',{id:'failed',type:'candidate-pair',state:'failed',localCandidateId:'l',remoteCandidateId:'r',requestsSent:41,responsesReceived:0});
  report.set('ok',{id:'ok',type:'candidate-pair',state:'succeeded',localCandidateId:'l',remoteCandidateId:'r',requestsSent:2,responsesReceived:2});
  const result=logic.iceConnectivity(report,{id:'ok'}), path=result.path_checks.p2p;
  assert.equal(result.pairs.length,16);assert.equal(result.pairs[1].id,'failed');
  assert.equal(path.pairs,32);assert.equal(path.attempted_pairs,2);assert.equal(path.responding_pairs,1);
  assert.equal(path.request_counter_pairs,2);assert.equal(path.response_counter_pairs,2);
  assert.equal(path.requests_sent,43);assert.equal(path.responses_received,2);assert.equal(path.failed_pairs,1);
  assert.equal(result.pairs[1].responses_received,0);assert.equal(result.pairs[2].requests_sent,null);
});
