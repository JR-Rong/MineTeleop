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
  const CONTROL_PROFILE_VERSION = 2;
  const CONTROL_PROFILE_FIELDS = Object.freeze([
    'profile_version',
    'target_speed_kph',
    'max_motor_torque_nm',
    'max_brake_pressure_bar',
    'service_brake_pressure_bar',
    'hard_brake_pressure_bar',
    'max_steering_angle_deg',
    'speed_pid_kp',
    'speed_pid_ki',
    'speed_pid_kd',
    'speed_pid_derivative_filter_tau_ms',
    'speed_pid_max_dt_ms',
  ]);
  const READ_ONLY_CONTROL_SAFETY_FIELDS = Object.freeze([
    'control_rate_hz',
    'max_command_gap_ms',
    'degraded_timeout_ms',
    'control_timeout_ms',
    'deceleration_profile',
    'speed_feedback_timeout_ms',
    'hard_overspeed_margin_kph',
    'require_can_feedback_before_control',
    'require_local_estop_reset',
    'require_time_sync',
    'max_time_sync_uncertainty_ms',
    'time_sync_interval_ms',
    'time_sync_samples',
    'commissioning_mode',
  ]);

  function clamp(value, minimum, maximum) {
    return Math.min(maximum, Math.max(minimum, value));
  }

  function finiteNumber(value, fallback = 0) {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
  }

  function requireFiniteRange(value, minimum, maximum, name) {
    if (typeof value !== 'number' || !Number.isFinite(value) ||
        value < minimum || value > maximum) {
      throw new TypeError(`${name} must be finite and in [${minimum}, ${maximum}]`);
    }
    return value;
  }

  function requireIntegerRange(value, minimum, maximum, name) {
    const number = requireFiniteRange(value, minimum, maximum, name);
    if (!Number.isSafeInteger(number)) throw new TypeError(`${name} must be an integer`);
    return number;
  }

  function normalizeSpeedPidLimits(value) {
    if (!value || typeof value !== 'object') {
      throw new TypeError('speed_pid_limits must be an object');
    }
    const normalizeRange = (rangeValue, minimum, maximum, name) => {
      if (!rangeValue || typeof rangeValue !== 'object') {
        throw new TypeError(`${name} must be an object`);
      }
      const normalized = {
        min: requireFiniteRange(rangeValue.min, minimum, maximum, `${name}.min`),
        max: requireFiniteRange(rangeValue.max, minimum, maximum, `${name}.max`),
      };
      if (normalized.min > normalized.max) throw new TypeError(`${name}.min must not exceed max`);
      return normalized;
    };
    const maxDt = normalizeRange(value.max_dt_ms, 20, 200, 'speed_pid_limits.max_dt_ms');
    if (!Number.isSafeInteger(maxDt.min) || !Number.isSafeInteger(maxDt.max)) {
      throw new TypeError('speed_pid_limits.max_dt_ms bounds must be integers');
    }
    return {
      kp: normalizeRange(value.kp, 0, 100, 'speed_pid_limits.kp'),
      ki: normalizeRange(value.ki, 0, 100, 'speed_pid_limits.ki'),
      kd: normalizeRange(value.kd, 0, 100, 'speed_pid_limits.kd'),
      derivative_filter_tau_ms: normalizeRange(
          value.derivative_filter_tau_ms, 0, 2000,
          'speed_pid_limits.derivative_filter_tau_ms'),
      max_dt_ms: maxDt,
    };
  }

  function normalizeReadOnlyControlSafety(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      throw new TypeError('read_only_control_safety must be an object');
    }
    const keys = Object.keys(value);
    if (keys.length !== READ_ONLY_CONTROL_SAFETY_FIELDS.length ||
        !READ_ONLY_CONTROL_SAFETY_FIELDS.every(
            field => Object.prototype.hasOwnProperty.call(value, field))) {
      throw new TypeError('read_only_control_safety must contain exactly the fixed fields');
    }
    const requireBoolean = (field) => {
      if (typeof value[field] !== 'boolean') {
        throw new TypeError(`read_only_control_safety.${field} must be boolean`);
      }
      return value[field];
    };
    const commissioningMode = value.commissioning_mode;
    if (typeof commissioningMode !== 'string' || commissioningMode.length < 1) {
      throw new TypeError(
          'read_only_control_safety.commissioning_mode must be a non-empty string');
    }
    if (!Array.isArray(value.deceleration_profile) ||
        value.deceleration_profile.length < 1) {
      throw new TypeError(
          'read_only_control_safety.deceleration_profile must be a non-empty array');
    }
    let previousAfterMs = -1;
    let previousBrake = 0;
    const decelerationProfile = value.deceleration_profile.map((stage, index) => {
      if (!stage || typeof stage !== 'object' || Array.isArray(stage) ||
          Object.keys(stage).length !== 2 ||
          !Object.prototype.hasOwnProperty.call(stage, 'after_ms') ||
          !Object.prototype.hasOwnProperty.call(stage, 'brake')) {
        throw new TypeError(
            'read_only_control_safety.deceleration_profile stages require exactly after_ms and brake');
      }
      const afterMs = requireIntegerRange(
          stage.after_ms, 0, 2147483647,
          `read_only_control_safety.deceleration_profile[${index}].after_ms`);
      const brake = requireFiniteRange(
          stage.brake, 0, 1,
          `read_only_control_safety.deceleration_profile[${index}].brake`);
      if ((index === 0 && afterMs !== 0) || afterMs <= previousAfterMs) {
        throw new TypeError(
            'read_only_control_safety.deceleration_profile after_ms must start at 0 and strictly increase');
      }
      if (brake < previousBrake) {
        throw new TypeError(
            'read_only_control_safety.deceleration_profile brake must not decrease');
      }
      previousAfterMs = afterMs;
      previousBrake = brake;
      return {after_ms: afterMs, brake};
    });
    if (Math.abs(
        decelerationProfile[decelerationProfile.length - 1].brake - 1) > 1e-9) {
      throw new TypeError(
          'read_only_control_safety.deceleration_profile must end at brake 1');
    }
    const normalized = {
      control_rate_hz: requireIntegerRange(
          value.control_rate_hz, 20, 20,
          'read_only_control_safety.control_rate_hz'),
      max_command_gap_ms: requireIntegerRange(
          value.max_command_gap_ms, 1, 60000,
          'read_only_control_safety.max_command_gap_ms'),
      degraded_timeout_ms: requireIntegerRange(
          value.degraded_timeout_ms, 1, 60000,
          'read_only_control_safety.degraded_timeout_ms'),
      control_timeout_ms: requireIntegerRange(
          value.control_timeout_ms, 1, 60000,
          'read_only_control_safety.control_timeout_ms'),
      deceleration_profile: decelerationProfile,
      speed_feedback_timeout_ms: requireIntegerRange(
          value.speed_feedback_timeout_ms, 20, 500,
          'read_only_control_safety.speed_feedback_timeout_ms'),
      hard_overspeed_margin_kph: requireFiniteRange(
          value.hard_overspeed_margin_kph, 0, 36,
          'read_only_control_safety.hard_overspeed_margin_kph'),
      require_can_feedback_before_control:
          requireBoolean('require_can_feedback_before_control'),
      require_local_estop_reset: requireBoolean('require_local_estop_reset'),
      require_time_sync: requireBoolean('require_time_sync'),
      max_time_sync_uncertainty_ms: requireIntegerRange(
          value.max_time_sync_uncertainty_ms, 0, 2147483647,
          'read_only_control_safety.max_time_sync_uncertainty_ms'),
      time_sync_interval_ms: requireIntegerRange(
          value.time_sync_interval_ms, 1, 2147483647,
          'read_only_control_safety.time_sync_interval_ms'),
      time_sync_samples: requireIntegerRange(
          value.time_sync_samples, 3, 15,
          'read_only_control_safety.time_sync_samples'),
      commissioning_mode: commissioningMode,
    };
    if (normalized.degraded_timeout_ms >= normalized.control_timeout_ms ||
        normalized.speed_feedback_timeout_ms > normalized.control_timeout_ms) {
      throw new TypeError(
          'read_only_control_safety timeout ordering is invalid');
    }
    if (normalized.hard_overspeed_margin_kph <= 0) {
      throw new TypeError(
          'read_only_control_safety.hard_overspeed_margin_kph must be greater than 0');
    }
    return normalized;
  }

  function normalizeControlProfile(value) {
    if (!value || typeof value !== 'object') throw new TypeError('control profile must be an object');
    const keys = Object.keys(value);
    if (keys.length !== CONTROL_PROFILE_FIELDS.length ||
        !CONTROL_PROFILE_FIELDS.every(
            field => Object.prototype.hasOwnProperty.call(value, field))) {
      throw new TypeError('control profile must contain exactly the V2 fields');
    }
    if (value.profile_version !== CONTROL_PROFILE_VERSION) {
      throw new TypeError(`profile_version must be ${CONTROL_PROFILE_VERSION}`);
    }
    const profile = {
      profile_version: CONTROL_PROFILE_VERSION,
      target_speed_kph: requireFiniteRange(value.target_speed_kph, 0, 72, 'target_speed_kph'),
      max_motor_torque_nm: requireFiniteRange(
          value.max_motor_torque_nm, 0, 640.0, 'max_motor_torque_nm'),
      max_brake_pressure_bar: requireFiniteRange(
          value.max_brake_pressure_bar, 0, 327.6, 'max_brake_pressure_bar'),
      service_brake_pressure_bar: requireFiniteRange(
          value.service_brake_pressure_bar, 0, 327.6, 'service_brake_pressure_bar'),
      hard_brake_pressure_bar: requireFiniteRange(
          value.hard_brake_pressure_bar, 0, 327.6, 'hard_brake_pressure_bar'),
      max_steering_angle_deg: requireFiniteRange(
          value.max_steering_angle_deg, 0, 30, 'max_steering_angle_deg'),
      speed_pid_kp: requireFiniteRange(value.speed_pid_kp, 0, 100, 'speed_pid_kp'),
      speed_pid_ki: requireFiniteRange(value.speed_pid_ki, 0, 100, 'speed_pid_ki'),
      speed_pid_kd: requireFiniteRange(value.speed_pid_kd, 0, 100, 'speed_pid_kd'),
      speed_pid_derivative_filter_tau_ms: requireFiniteRange(
          value.speed_pid_derivative_filter_tau_ms, 0, 2000,
          'speed_pid_derivative_filter_tau_ms'),
      speed_pid_max_dt_ms: requireIntegerRange(
          value.speed_pid_max_dt_ms, 20, 200, 'speed_pid_max_dt_ms'),
    };
    if (profile.speed_pid_kp <= 0) throw new TypeError('speed_pid_kp must be greater than 0');
    if (profile.service_brake_pressure_bar > profile.hard_brake_pressure_bar ||
        profile.hard_brake_pressure_bar > profile.max_brake_pressure_bar) {
      throw new TypeError(
          'brake pressures must satisfy service_brake_pressure_bar <= hard_brake_pressure_bar <= max_brake_pressure_bar');
    }
    return profile;
  }

  function normalizeVehicleHardLimits(value) {
    if (!value || typeof value !== 'object') throw new TypeError('vehicle hard limits must be an object');
    const maxSpeedKph = requireFiniteRange(value.max_speed_kph, 0, 72, 'max_speed_kph');
    const maxThrottle = requireFiniteRange(value.max_throttle, 0, 1, 'max_throttle');
    const computedTargetSpeedKph = maxSpeedKph * maxThrottle;
    const explicitTargetSpeedKph = typeof value.max_target_speed_kph === 'number'
        ? requireFiniteRange(value.max_target_speed_kph, 0, 72, 'max_target_speed_kph')
        : computedTargetSpeedKph;
    const speedPidLimits = normalizeSpeedPidLimits(value.speed_pid_limits);
    const readOnlyControlSafety =
        normalizeReadOnlyControlSafety(value.read_only_control_safety);
    const normalized = {
      max_speed_kph: maxSpeedKph,
      max_throttle: maxThrottle,
      max_target_speed_kph: Math.min(
          maxSpeedKph, computedTargetSpeedKph, explicitTargetSpeedKph),
      full_scale_motor_torque_nm: requireFiniteRange(
          value.full_scale_motor_torque_nm, 0, 640.0, 'full_scale_motor_torque_nm'),
      max_brake_pressure_bar: requireFiniteRange(
          value.max_brake_pressure_bar, 0, 327.6, 'max_brake_pressure_bar'),
      max_steering_angle_deg: requireFiniteRange(
          value.max_steering_angle_deg, 0, 30, 'max_steering_angle_deg'),
      default_speed_pid_kp: requireFiniteRange(
          value.default_speed_pid_kp, speedPidLimits.kp.min, speedPidLimits.kp.max,
          'default_speed_pid_kp'),
      default_speed_pid_ki: requireFiniteRange(
          value.default_speed_pid_ki, speedPidLimits.ki.min, speedPidLimits.ki.max,
          'default_speed_pid_ki'),
      default_speed_pid_kd: requireFiniteRange(
          value.default_speed_pid_kd, speedPidLimits.kd.min, speedPidLimits.kd.max,
          'default_speed_pid_kd'),
      default_speed_pid_derivative_filter_tau_ms: requireFiniteRange(
          value.default_speed_pid_derivative_filter_tau_ms,
          speedPidLimits.derivative_filter_tau_ms.min,
          speedPidLimits.derivative_filter_tau_ms.max,
          'default_speed_pid_derivative_filter_tau_ms'),
      default_speed_pid_max_dt_ms: requireIntegerRange(
          value.default_speed_pid_max_dt_ms, speedPidLimits.max_dt_ms.min,
          speedPidLimits.max_dt_ms.max, 'default_speed_pid_max_dt_ms'),
      speed_pid_limits: speedPidLimits,
      speed_feedback_timeout_ms: requireIntegerRange(
          value.speed_feedback_timeout_ms, 20, 500, 'speed_feedback_timeout_ms'),
      hard_overspeed_margin_kph: requireFiniteRange(
          value.hard_overspeed_margin_kph, 0, 36, 'hard_overspeed_margin_kph'),
      speed_feedback_timeout_ms_read_only:
          value.speed_feedback_timeout_ms_read_only,
      hard_overspeed_margin_kph_read_only:
          value.hard_overspeed_margin_kph_read_only,
      read_only_control_safety: readOnlyControlSafety,
    };
    if (normalized.default_speed_pid_kp <= 0) {
      throw new TypeError('default_speed_pid_kp must be greater than 0');
    }
    if (normalized.hard_overspeed_margin_kph <= 0) {
      throw new TypeError('hard_overspeed_margin_kph must be greater than 0');
    }
    if (value.speed_feedback_timeout_ms_read_only !== true ||
        value.hard_overspeed_margin_kph_read_only !== true) {
      throw new TypeError('vehicle speed safety fields must be explicitly read-only');
    }
    if (normalized.speed_feedback_timeout_ms !==
            readOnlyControlSafety.speed_feedback_timeout_ms ||
        normalized.hard_overspeed_margin_kph !==
            readOnlyControlSafety.hard_overspeed_margin_kph) {
      throw new TypeError('flat and read_only_control_safety speed values must match');
    }
    return normalized;
  }

  function mergeControlProfileWithHardLimits(requestedValue, hardLimitValue) {
    const requested = normalizeControlProfile(requestedValue);
    const hard = normalizeVehicleHardLimits(hardLimitValue);
    const maxBrake = Math.min(
        requested.max_brake_pressure_bar, hard.max_brake_pressure_bar);
    return {
      profile_version: CONTROL_PROFILE_VERSION,
      target_speed_kph: Math.min(requested.target_speed_kph, hard.max_target_speed_kph),
      max_motor_torque_nm: Math.min(
          requested.max_motor_torque_nm, hard.full_scale_motor_torque_nm),
      max_brake_pressure_bar: maxBrake,
      service_brake_pressure_bar: Math.min(requested.service_brake_pressure_bar, maxBrake),
      hard_brake_pressure_bar: Math.min(requested.hard_brake_pressure_bar, maxBrake),
      max_steering_angle_deg: Math.min(
          requested.max_steering_angle_deg, hard.max_steering_angle_deg),
      speed_pid_kp: clamp(
          requested.speed_pid_kp, hard.speed_pid_limits.kp.min,
          hard.speed_pid_limits.kp.max),
      speed_pid_ki: clamp(
          requested.speed_pid_ki, hard.speed_pid_limits.ki.min,
          hard.speed_pid_limits.ki.max),
      speed_pid_kd: clamp(
          requested.speed_pid_kd, hard.speed_pid_limits.kd.min,
          hard.speed_pid_limits.kd.max),
      speed_pid_derivative_filter_tau_ms: clamp(
          requested.speed_pid_derivative_filter_tau_ms,
          hard.speed_pid_limits.derivative_filter_tau_ms.min,
          hard.speed_pid_limits.derivative_filter_tau_ms.max),
      speed_pid_max_dt_ms: clamp(
          requested.speed_pid_max_dt_ms, hard.speed_pid_limits.max_dt_ms.min,
          hard.speed_pid_limits.max_dt_ms.max),
    };
  }

  function controlProfileFromVehicleDefaults(actuationValue, hardLimitValue) {
    if (!actuationValue || typeof actuationValue !== 'object') {
      throw new TypeError('control actuation defaults must be an object');
    }
    const hard = normalizeVehicleHardLimits(hardLimitValue);
    return mergeControlProfileWithHardLimits({
      profile_version: CONTROL_PROFILE_VERSION,
      target_speed_kph: actuationValue.target_speed_kph,
      max_motor_torque_nm: actuationValue.max_motor_torque_nm,
      max_brake_pressure_bar: actuationValue.max_brake_pressure_bar,
      service_brake_pressure_bar: actuationValue.service_brake_pressure_bar,
      hard_brake_pressure_bar: actuationValue.hard_brake_pressure_bar,
      max_steering_angle_deg: actuationValue.max_steering_angle_deg,
      speed_pid_kp: hard.default_speed_pid_kp,
      speed_pid_ki: hard.default_speed_pid_ki,
      speed_pid_kd: hard.default_speed_pid_kd,
      speed_pid_derivative_filter_tau_ms:
          hard.default_speed_pid_derivative_filter_tau_ms,
      speed_pid_max_dt_ms: hard.default_speed_pid_max_dt_ms,
    }, hard);
  }

  function controlProfileThrottleLimit(profileValue, hardLimitValue) {
    const profile = normalizeControlProfile(profileValue);
    const hard = normalizeVehicleHardLimits(hardLimitValue);
    if (hard.max_speed_kph === 0) return 0;
    return clamp(profile.target_speed_kph / hard.max_speed_kph, 0, hard.max_throttle);
  }

  function controlProfilesEqual(leftValue, rightValue) {
    let left;
    let right;
    try {
      left = normalizeControlProfile(leftValue);
      right = normalizeControlProfile(rightValue);
    } catch (_) {
      return false;
    }
    return left.target_speed_kph === right.target_speed_kph &&
        left.max_motor_torque_nm === right.max_motor_torque_nm &&
        left.max_brake_pressure_bar === right.max_brake_pressure_bar &&
        left.service_brake_pressure_bar === right.service_brake_pressure_bar &&
        left.hard_brake_pressure_bar === right.hard_brake_pressure_bar &&
        left.max_steering_angle_deg === right.max_steering_angle_deg &&
        left.speed_pid_kp === right.speed_pid_kp &&
        left.speed_pid_ki === right.speed_pid_ki &&
        left.speed_pid_kd === right.speed_pid_kd &&
        left.speed_pid_derivative_filter_tau_ms ===
            right.speed_pid_derivative_filter_tau_ms &&
        left.speed_pid_max_dt_ms === right.speed_pid_max_dt_ms;
  }

  function reduceControlProfileStatus(stateValue, statusValue) {
    const state = Object.assign({
      requestedProfile: null,
      pendingRequestSeq: 0,
      effectiveProfile: null,
      effectiveRequestSeq: 0,
      effectiveAppliedRevision: 0,
      acknowledged: false,
      reason: '',
    }, stateValue || {});
    const status = statusValue && typeof statusValue === 'object' ? statusValue : {};
    const requestSeq = status.last_request_seq;
    const pendingRequestSeq = state.pendingRequestSeq;
    const effectiveRequestSeq = state.effectiveRequestSeq;
    const appliedRevision = status.applied_revision;
    if (Number.isSafeInteger(pendingRequestSeq) && pendingRequestSeq > 0) {
      if (!Number.isSafeInteger(requestSeq) || requestSeq !== pendingRequestSeq) {
        return {...state, matched: false, invalidated: false};
      }
      if (status.accepted !== true || status.active !== true) {
        return {
          ...state,
          pendingRequestSeq: 0,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: String(status.reason || 'profile_rejected'),
          matched: true,
          invalidated: true,
        };
      }
      if (!Number.isSafeInteger(appliedRevision) || appliedRevision <= 0 ||
          appliedRevision !== pendingRequestSeq) {
        return {
          ...state,
          pendingRequestSeq: 0,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: 'applied_revision_mismatch',
          matched: true,
          invalidated: true,
        };
      }
      let effectiveProfile;
      try {
        effectiveProfile = normalizeControlProfile(status.effective_profile);
      } catch (error) {
        return {
          ...state,
          pendingRequestSeq: 0,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: `invalid_effective_profile: ${error.message}`,
          matched: true,
          invalidated: true,
        };
      }
      if (!controlProfilesEqual(effectiveProfile, state.requestedProfile)) {
        return {
          ...state,
          pendingRequestSeq: 0,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: 'effective_profile_mismatch',
          matched: true,
          invalidated: true,
        };
      }
      return {
        ...state,
        pendingRequestSeq: 0,
        effectiveProfile,
        effectiveRequestSeq: requestSeq,
        effectiveAppliedRevision: appliedRevision,
        acknowledged: true,
        reason: String(status.reason || 'accepted'),
        matched: true,
        invalidated: false,
      };
    }

    if (state.acknowledged === true) {
      if (status.accepted !== true || status.active !== true ||
          !Number.isSafeInteger(requestSeq) ||
          requestSeq !== effectiveRequestSeq ||
          !Number.isSafeInteger(appliedRevision) || appliedRevision <= 0 ||
          appliedRevision !== state.effectiveAppliedRevision) {
        return {
          ...state,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: String(status.reason || 'profile_inactive_or_replaced'),
          matched: true,
          invalidated: true,
        };
      }
      let effectiveProfile;
      try {
        effectiveProfile = normalizeControlProfile(status.effective_profile);
      } catch (error) {
        return {
          ...state,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: `invalid_effective_profile: ${error.message}`,
          matched: true,
          invalidated: true,
        };
      }
      if (!controlProfilesEqual(effectiveProfile, state.effectiveProfile)) {
        return {
          ...state,
          effectiveProfile: null,
          effectiveRequestSeq: 0,
          effectiveAppliedRevision: 0,
          acknowledged: false,
          reason: 'effective_profile_changed',
          matched: true,
          invalidated: true,
        };
      }
      return {
        ...state,
        effectiveProfile,
        reason: String(status.reason || state.reason || 'accepted'),
        matched: true,
        invalidated: false,
      };
    }
    return {...state, matched: false, invalidated: false};
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
    const maxBrakePressureBar = clamp(
        finiteNumber(activeLimits.maxBrakePressureBar), 0, 327.6);
    const hardBrakePressureBar = clamp(
        finiteNumber(activeLimits.hardBrakePressureBar), 0, maxBrakePressureBar);
    const serviceBrakePressureBar = clamp(
        finiteNumber(activeLimits.serviceBrakePressureBar), 0, hardBrakePressureBar);
    const hardBrake = maxBrakePressureBar > 0
        ? hardBrakePressureBar / maxBrakePressureBar : 0;
    const serviceBrake = maxBrakePressureBar > 0
        ? serviceBrakePressureBar / maxBrakePressureBar : 0;
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
    let brake = maxBrakePressureBar > 0 ? gamepadBrake : 0;
    if (state.service_brake) brake = Math.max(brake, serviceBrake);
    if (state.hard_brake) brake = Math.max(brake, hardBrake);
    if (state.service_brake || state.hard_brake || gamepadBrake > 0) throttle = 0;

    return {
      gear,
      steering: clamp(steering, -1, 1) * (maxSteeringDeg / steeringScale),
      throttle: throttle * maxThrottle,
      brake: clamp(brake, 0, maxBrakePressureBar > 0 ? 1 : 0),
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

  function deriveControlCommandRejection(issueCodeValue) {
    const issueCode = String(issueCodeValue || '');
    if (issueCode === 'vcu_drive_gear_change_moving_or_stale') {
      return {
        issueCode,
        clearInput: true,
        severity: 'critical',
        text: '换挡被车端拒绝：车辆仍在移动，或挡位/速度反馈已过期；请停车并恢复新鲜反馈后，释放再重新选择 D/R。',
      };
    }
    return {
      issueCode: 'vcu_control_apply_rejected',
      clearInput: true,
      severity: 'critical',
      text: '控制命令被车端拒绝，车辆已保持安全状态；请检查车端 VCU 日志 issue_code 后再重试。',
    };
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
    normalizeControlProfile,
    normalizeVehicleHardLimits,
    mergeControlProfileWithHardLimits,
    controlProfileFromVehicleDefaults,
    controlProfileThrottleLimit,
    reduceControlProfileStatus,
    reduceGamepadNeutralInterlock,
    reduceStatusSequence,
    deriveEstopPresentation,
    deriveControlCommandRejection,
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
