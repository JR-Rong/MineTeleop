'use strict';

(function exposeControlLogic(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MineTeleopControlLogic = api;
})(typeof globalThis === 'undefined' ? this : globalThis, function createControlLogic() {
  const KEY_BINDINGS = Object.freeze({
    ArrowLeft: 'left',
    KeyA: 'left',
    ArrowRight: 'right',
    KeyD: 'right',
    ArrowUp: 'up',
    KeyW: 'up',
    ArrowDown: 'down',
    KeyS: 'down',
    Space: 'service_brake',
    KeyB: 'hard_brake',
  });
  const CONTROL_ACTIONS = Object.freeze([
    'left',
    'right',
    'up',
    'down',
    'service_brake',
    'hard_brake',
  ]);

  function clamp(value, minimum, maximum) {
    return Math.min(maximum, Math.max(minimum, value));
  }

  function finiteNumber(value, fallback = 0) {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
  }

  function requireKeySet(value, name) {
    if (!(value instanceof Set)) throw new TypeError(`${name} must be a Set`);
  }

  function createKeySet(values = []) {
    return new Set(values);
  }

  function keyAction(code) {
    return KEY_BINDINGS[String(code)] || null;
  }

  function blockKey(blockedCodes, code) {
    requireKeySet(blockedCodes, 'blockedCodes');
    if (!keyAction(code)) return false;
    blockedCodes.add(code);
    return true;
  }

  function pressKey(pressedCodes, blockedCodes, code) {
    requireKeySet(pressedCodes, 'pressedCodes');
    requireKeySet(blockedCodes, 'blockedCodes');
    const action = keyAction(code);
    if (!action) return {accepted: false, changed: false, reason: 'unmapped', action: null};
    if (blockedCodes.has(code)) {
      return {accepted: false, changed: false, reason: 'blocked_until_release', action};
    }
    if (pressedCodes.has(code)) {
      return {accepted: true, changed: false, reason: 'repeat', action};
    }
    pressedCodes.add(code);
    return {accepted: true, changed: true, reason: 'pressed', action};
  }

  function releaseKey(pressedCodes, blockedCodes, code) {
    requireKeySet(pressedCodes, 'pressedCodes');
    requireKeySet(blockedCodes, 'blockedCodes');
    const action = keyAction(code);
    if (!action) return {mapped: false, changed: false, action: null};
    blockedCodes.delete(code);
    return {mapped: true, changed: pressedCodes.delete(code), action};
  }

  function blockAndClearKeys(pressedCodes, blockedCodes) {
    requireKeySet(pressedCodes, 'pressedCodes');
    requireKeySet(blockedCodes, 'blockedCodes');
    for (const code of pressedCodes) blockedCodes.add(code);
    pressedCodes.clear();
  }

  function deriveKeyState(pressedCodes) {
    requireKeySet(pressedCodes, 'pressedCodes');
    const state = {};
    for (const action of CONTROL_ACTIONS) state[action] = false;
    for (const code of pressedCodes) {
      const action = keyAction(code);
      if (action) state[action] = true;
    }
    return state;
  }

  function mockUnsupported(value) {
    return Boolean(value) && !value.supported && value.state === 'unsupported';
  }

  function adapterReady(status, explicit) {
    if (explicit === true || explicit === false) return explicit;
    const state = (status && status.state) || 'unavailable';
    if (state === 'unsupported') return true;
    if (['unavailable', 'closed', 'fault'].includes(state)) return null;
    return true;
  }

  function drivingReady(value) {
    const handshakeReady = Boolean(value && value.ready) || mockUnsupported(value);
    return Boolean(value) && value.adapter_ready === true && handshakeReady;
  }

  function requiresFreshInput(value) {
    const state = String((value && value.state) || '');
    return state === 'fault' || state === 'closed' || state === 'disarmed' || state.startsWith('disarm_');
  }

  function keepsHeldInput(everReady, value) {
    return Boolean(everReady) && Boolean(value) && value.adapter_ready === true &&
        ['wait_gear', 'wait_actuator_modes'].includes(String(value.state || ''));
  }

  function transitionVcuState(everReady, nextValue) {
    const previouslyReady = Boolean(everReady);
    const readyNow = drivingReady(nextValue);
    const nextEverReady = previouslyReady || readyNow;
    if (requiresFreshInput(nextValue)) return {everReady: false, resetInput: true, retainedWait: false};
    if ((!nextValue || nextValue.adapter_ready !== true) && previouslyReady) {
      return {everReady: false, resetInput: true, retainedWait: false};
    }
    const retainedWait = keepsHeldInput(nextEverReady, nextValue);
    if (nextEverReady && !readyNow && !retainedWait) {
      return {everReady: false, resetInput: true, retainedWait: false};
    }
    return {everReady: nextEverReady, resetInput: false, retainedWait};
  }

  function allowsGearChange(selectedGear, requestedGear, vcuStatus) {
    if (selectedGear === requestedGear || mockUnsupported(vcuStatus)) return true;
    const speed = Number(vcuStatus && vcuStatus.speed_mps);
    return Boolean(vcuStatus && vcuStatus.speed_valid) &&
        Number.isFinite(speed) && Math.abs(speed) <= 0.1;
  }

  function deriveGearSelection(selectedGear, keyState, vcuStatus) {
    const currentGear = ['N', 'R', 'D'].includes(selectedGear) ? selectedGear : 'N';
    if (Boolean(keyState && keyState.up) === Boolean(keyState && keyState.down)) {
      return {selectedGear: currentGear, pendingGearRequest: null, changed: false};
    }
    const requestedGear = keyState.up ? 'D' : 'R';
    if (!allowsGearChange(currentGear, requestedGear, vcuStatus)) {
      return {selectedGear: currentGear, pendingGearRequest: requestedGear, changed: false};
    }
    return {
      selectedGear: requestedGear,
      pendingGearRequest: null,
      changed: requestedGear !== currentGear,
    };
  }

  function deriveControl({keyState, gamepad, selectedGear, limits, steeringFullScaleDeg, estop = false}) {
    const state = keyState || {};
    const pad = gamepad || {};
    const activeLimits = limits || {};
    const maxThrottle = clamp(finiteNumber(activeLimits.maxThrottle), 0, 1);
    const hardBrake = clamp(finiteNumber(activeLimits.hardBrake), 0, 1);
    const serviceBrake = clamp(finiteNumber(activeLimits.serviceBrake), 0, hardBrake);
    const maxSteeringDeg = Math.max(0, finiteNumber(activeLimits.maxSteeringDeg));
    const steeringScale = Math.max(Number.EPSILON, finiteNumber(steeringFullScaleDeg, 1));
    const gear = ['N', 'R', 'D'].includes(selectedGear) ? selectedGear : 'N';

    let steering = finiteNumber(pad.steering);
    if (state.left || state.right) steering = state.left === state.right ? 0 : (state.left ? -1 : 1);

    let throttle = clamp(finiteNumber(pad.throttle), 0, 1);
    const requestedGear = state.up !== state.down ? (state.up ? 'D' : 'R') : null;
    if (requestedGear) throttle = requestedGear === gear ? 1 : 0;
    else if (state.up && state.down) throttle = 0;
    if (gear === 'N') throttle = 0;

    const gamepadBrake = clamp(finiteNumber(pad.brake), 0, 1);
    let brake = gamepadBrake * hardBrake;
    if (state.service_brake) brake = Math.max(brake, serviceBrake);
    if (state.hard_brake) brake = hardBrake;
    if (state.service_brake || state.hard_brake || gamepadBrake > 0) throttle = 0;

    return {
      gear,
      steering: clamp(steering, -1, 1) * (maxSteeringDeg / steeringScale),
      throttle: throttle * maxThrottle,
      brake: clamp(brake, 0, hardBrake),
      estop: Boolean(estop),
    };
  }

  function reduceGamepadNeutralInterlock({
    requiresNeutral,
    authorityReady,
    throttle,
    brake,
    gearRequestPending = false,
  }) {
    const sampledThrottle = clamp(finiteNumber(throttle), 0, 1);
    const sampledBrake = clamp(finiteNumber(brake), 0, 1);
    if (!authorityReady || gearRequestPending) {
      return {requiresNeutral: true, throttle: 0, brake: 0};
    }
    if (requiresNeutral) {
      const pedalsNeutral = sampledThrottle === 0 && sampledBrake === 0;
      return {requiresNeutral: !pedalsNeutral, throttle: 0, brake: 0};
    }
    return {requiresNeutral: false, throttle: sampledThrottle, brake: sampledBrake};
  }

  function reduceStatusSequence(lastSequence, candidateSequence) {
    const last = Number(lastSequence);
    const candidate = Number(candidateSequence);
    if (!Number.isSafeInteger(last) || last < 0) throw new TypeError('lastSequence must be a non-negative safe integer');
    if (!Number.isSafeInteger(candidate) || candidate <= last) {
      return {accepted: false, lastSequence: last, gap: 0};
    }
    return {accepted: true, lastSequence: candidate, gap: Math.max(0, candidate - last - 1)};
  }

  function deriveEstopPresentation(localLatched, vehicleActive) {
    const local = Boolean(localLatched);
    const vehicle = Boolean(vehicleActive);
    if (local && vehicle) {
      return {
        kind: 'local_confirmed',
        visible: true,
        severity: 'critical',
        banner: '车辆急停已由车端遥测确认；车辆必须本地确认后才能复位。',
        alert: '车辆急停已由车端遥测确认，仍需本地确认后复位',
      };
    }
    if (local) {
      return {
        kind: 'local_pending',
        visible: true,
        severity: 'critical',
        banner: '急停请求已锁定；等待车端遥测确认，未确认时请使用车辆物理急停。',
        alert: '急停请求已锁定但尚未收到车端确认；请准备使用车辆物理急停',
      };
    }
    if (vehicle) {
      return {
        kind: 'vehicle_only',
        visible: true,
        severity: 'critical',
        banner: '车端遥测报告车辆已急停；该急停不是由本页面请求，请立即核实车辆现场状态。',
        alert: '车端遥测报告车辆已急停；不是由本页面请求，请立即核实车辆现场状态',
      };
    }
    return {kind: 'none', visible: false, severity: '', banner: '', alert: ''};
  }

  function isCurrentPeer(activePeer, callbackPeer) {
    return Boolean(activePeer) && activePeer === callbackPeer;
  }

  function isCurrentControlChannel(activePeer, callbackPeer, activeChannel, callbackChannel) {
    return isCurrentPeer(activePeer, callbackPeer) &&
        Boolean(activeChannel) && activeChannel === callbackChannel;
  }

  function controlSnapshot(value) {
    return {
      gear: (value && value.gear) || 'N',
      steering: finiteNumber(value && value.steering),
      throttle: finiteNumber(value && value.throttle),
      brake: finiteNumber(value && value.brake),
      estop: Boolean(value && value.estop),
    };
  }

  function controlIntentSuperseded(preparedSnapshot, latestValue) {
    const prepared = controlSnapshot(preparedSnapshot);
    if (prepared.estop) return false;
    return JSON.stringify(prepared) !== JSON.stringify(controlSnapshot(latestValue));
  }

  const CONTROL_OUTCOMES = Object.freeze([
    'forwarded',
    'superseded',
    'post_prepare_link_changed',
    'post_prepare_vcu_not_ready',
  ]);

  function shouldLogControlOutcome(outcome) {
    if (outcome === 'prepared' || outcome === 'forwarded') return false;
    if (!CONTROL_OUTCOMES.includes(outcome)) throw new TypeError('unknown control outcome');
    return true;
  }

  function createControlOutcomeMetrics() {
    return {
      prepared: 0,
      forwarded: 0,
      superseded: 0,
      post_prepare_link_changed: 0,
      post_prepare_vcu_not_ready: 0,
      last_prepared_seq: 0,
      last_forwarded_seq: 0,
    };
  }

  function terminalControlOutcomeCount(metrics) {
    return CONTROL_OUTCOMES.reduce(function sumOutcomes(total, name) {
      return total + Number(metrics[name] || 0);
    }, 0);
  }

  function reduceControlOutcome(metrics, outcome, sequence) {
    const current = Object.assign(createControlOutcomeMetrics(), metrics || {});
    const seq = Number(sequence);
    if (!Number.isSafeInteger(seq) || seq <= 0) {
      throw new TypeError('control sequence must be a positive safe integer');
    }
    const terminalCount = terminalControlOutcomeCount(current);
    if (outcome === 'prepared') {
      if (terminalCount !== current.prepared) {
        throw new Error('previous prepared control has no terminal browser outcome');
      }
      if (seq <= current.last_prepared_seq) {
        throw new Error('prepared control sequence must increase');
      }
      current.prepared += 1;
      current.last_prepared_seq = seq;
      return current;
    }
    if (!CONTROL_OUTCOMES.includes(outcome)) throw new TypeError('unknown control outcome');
    if (terminalCount + 1 !== current.prepared || seq !== current.last_prepared_seq) {
      throw new Error('terminal browser outcome does not match the prepared control');
    }
    current[outcome] += 1;
    if (outcome === 'forwarded') current.last_forwarded_seq = seq;
    return current;
  }

  function controlOutcomesBalanced(metrics) {
    const current = Object.assign(createControlOutcomeMetrics(), metrics || {});
    return terminalControlOutcomeCount(current) === current.prepared;
  }

  return Object.freeze({
    KEY_BINDINGS,
    CONTROL_ACTIONS,
    createKeySet,
    keyAction,
    blockKey,
    pressKey,
    releaseKey,
    blockAndClearKeys,
    deriveKeyState,
    mockUnsupported,
    adapterReady,
    drivingReady,
    requiresFreshInput,
    keepsHeldInput,
    transitionVcuState,
    allowsGearChange,
    deriveGearSelection,
    deriveControl,
    reduceGamepadNeutralInterlock,
    reduceStatusSequence,
    deriveEstopPresentation,
    isCurrentPeer,
    isCurrentControlChannel,
    controlSnapshot,
    controlIntentSuperseded,
    CONTROL_OUTCOMES,
    shouldLogControlOutcome,
    createControlOutcomeMetrics,
    reduceControlOutcome,
    controlOutcomesBalanced,
  });
});
