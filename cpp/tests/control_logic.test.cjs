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
    command_seq: 101,
    control_status_seq: 41,
  }, 'R'), true);
  assert.equal(logic.matchesGearChangeRejection(transition, {
    issue_code: 'vcu_drive_gear_change_moving_or_stale',
    command_seq: 103,
    control_status_seq: 41,
  }, 'R'), false);
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
    command_seq: 101,
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
    profile_version: 3,
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

test('session control profile V3 validation and hard-limit merge preserve ordering', () => {
  const requested = controlProfile();
  const hard = vehicleHardLimits();
  assert.deepEqual(logic.mergeControlProfileWithHardLimits(requested, hard), {
    profile_version: 3,
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
      /profile_version must be 3/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, profile_version: 2}),
      /profile_version must be 3/);
  assert.throws(
      () => logic.normalizeControlProfile({...requested, unexpected_field: 1}),
      /exactly the V3 fields/);
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
      /exactly the V3 fields/);
});

test('PID defaults come only from complete vehicle limits and accept kp hard min zero', () => {
  const hard = vehicleHardLimits({
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
        deferred[0]?.resolve({sent: false, reason: 'control_prepare_preempted_by_estop'});
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

if (failed > 0) {
  console.error(`${failed} of ${passed + failed} control logic tests failed`);
  process.exitCode = 1;
} else {
  console.log(`${passed} control logic tests passed`);
}
