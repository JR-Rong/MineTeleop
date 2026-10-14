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
  const GEAR_CHANGE_STATIONARY_SPEED_MPS = 0.1;
  const GEAR_CHANGE_STATIONARY_MIN_SAMPLES = 3;
  const GEAR_CHANGE_STATIONARY_MIN_DURATION_MS = 200;
  const CONTROL_PROFILE_VERSION = 7;
  const CONTROL_PROFILE_FIELDS = Object.freeze([
    'profile_version',
    'parking_idle_timeout_ms',
    'consecutive_late_frame_limit',
    'critical_camera_stale_brake_ms',
    'control_recovery_timeout_ms',
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
    'motor_torque_rise_rate_nm_per_s',
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
      throw new TypeError('control profile must contain exactly the V7 fields');
    }
    if (value.profile_version !== CONTROL_PROFILE_VERSION) {
      throw new TypeError(`profile_version must be ${CONTROL_PROFILE_VERSION}`);
    }
    const profile = {
      profile_version: CONTROL_PROFILE_VERSION,
      consecutive_late_frame_limit: requireIntegerRange(value.consecutive_late_frame_limit, 1, 5, 'consecutive_late_frame_limit'),
      parking_idle_timeout_ms: requireIntegerRange(value.parking_idle_timeout_ms, 0, 1000, 'parking_idle_timeout_ms'),
      critical_camera_stale_brake_ms: requireIntegerRange(value.critical_camera_stale_brake_ms, 100, 3000, 'critical_camera_stale_brake_ms'),
      control_recovery_timeout_ms: requireIntegerRange(value.control_recovery_timeout_ms, 1000, 30000, 'control_recovery_timeout_ms'),
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
      motor_torque_rise_rate_nm_per_s: requireFiniteRange(
          value.motor_torque_rise_rate_nm_per_s, 0, 32000,
          'motor_torque_rise_rate_nm_per_s'),
    };
    if (profile.speed_pid_kp <= 0) throw new TypeError('speed_pid_kp must be greater than 0');
    if (profile.service_brake_pressure_bar > profile.hard_brake_pressure_bar ||
        profile.hard_brake_pressure_bar > profile.max_brake_pressure_bar) {
      throw new TypeError(
          'brake pressures must satisfy service_brake_pressure_bar <= hard_brake_pressure_bar <= max_brake_pressure_bar');
    }
    return profile;
  }

  function vehicleControlDiagnostic(issueCode, recovery) {
    const causes = {time_sync: '时间同步不可信', input_lease: '驾驶输入刷新过期',
      control_link: '控制链路 ACK 确认中断', critical_video: '关键视频中断或无新帧'};
    const cause = recovery && causes[recovery.cause];
    const timeout = recovery && Number(recovery.timeout_ms);
    const deadline = Number.isFinite(timeout) && timeout > 0 ? `${timeout / 1000} 秒` : '配置期限';
    if (issueCode === 'control_recovery_braking_wait') return {
      level: 'warn', title: '控制暂缓，制动等待恢复',
      text: `${cause ? '原因：' + cause + '。' : ''}牵引已撤销，车辆正在制动；当前会话与参数保留。故障恢复后保持制动等待新操作；持续超过${deadline}将安全停车。`,
    };
    if (issueCode === 'control_recovery_timeout') return {
      level: 'critical', title: cause ? `${cause}，恢复超时` : '控制故障持续超时，控制已锁定',
      text: `${cause || '控制就绪条件'}持续超过${deadline}未稳定恢复。车辆已安全停车、控制已锁定；请结束会话，待故障恢复后重新连接。`,
    };
    if (issueCode === 'critical_camera_failed') return {
      level: 'critical', title: '关键视频故障，控制已锁定',
      text: '关键视频中断或启动超时，本会话已禁止控制。画面恢复不会解除锁定；请结束会话，待视频正常后重新连接并确认限幅、完成 VCU 握手。',
    };
    if (issueCode === 'critical_camera_not_ready') return {
      level: 'warn', title: '等待关键视频就绪',
      text: '正在等待关键视频编码输出，VCU 适配器尚未启动；视频就绪后将继续准备控制，当前不能握手或驾驶。',
    };
    if (issueCode === 'vcu_adapter_start_failed') return {
      level: 'critical', title: 'VCU 适配器启动失败',
      text: '车端 VCU 适配器未能启动，请检查 CAN 配置、桥接库及车端日志；驾驶保持禁用。',
    };
    return null;
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
    // Pre-V3 vehicles do not publish the rise-rate keys; treat them like any
    // other incomplete hard-limits report and fail closed.
    if (!value.motor_torque_rise_rate_limits_nm_per_s ||
        typeof value.motor_torque_rise_rate_limits_nm_per_s !== 'object') {
      throw new TypeError('motor_torque_rise_rate_limits_nm_per_s must be an object');
    }
    const riseRateLimitsValue = value.motor_torque_rise_rate_limits_nm_per_s;
    const riseRateLimits = {
      min: requireFiniteRange(
          riseRateLimitsValue.min, 0, 32000,
          'motor_torque_rise_rate_limits_nm_per_s.min'),
      max: requireFiniteRange(
          riseRateLimitsValue.max, 0, 32000,
          'motor_torque_rise_rate_limits_nm_per_s.max'),
    };
    if (riseRateLimits.min > riseRateLimits.max) {
      throw new TypeError(
          'motor_torque_rise_rate_limits_nm_per_s.min must not exceed max');
    }
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
      default_critical_camera_stale_brake_ms: requireIntegerRange(value.default_critical_camera_stale_brake_ms, 100, 3000, 'default_critical_camera_stale_brake_ms'),
      default_control_recovery_timeout_ms: requireIntegerRange(value.default_control_recovery_timeout_ms, 1000, 30000, 'default_control_recovery_timeout_ms'),
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
      default_motor_torque_rise_rate_nm_per_s: requireFiniteRange(
          value.default_motor_torque_rise_rate_nm_per_s,
          riseRateLimits.min, riseRateLimits.max,
          'default_motor_torque_rise_rate_nm_per_s'),
      motor_torque_rise_rate_limits_nm_per_s: riseRateLimits,
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
      parking_idle_timeout_ms: requested.parking_idle_timeout_ms,
      consecutive_late_frame_limit: requested.consecutive_late_frame_limit,
      critical_camera_stale_brake_ms: requested.critical_camera_stale_brake_ms,
      control_recovery_timeout_ms: requested.control_recovery_timeout_ms,
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
      motor_torque_rise_rate_nm_per_s: clamp(
          requested.motor_torque_rise_rate_nm_per_s,
          hard.motor_torque_rise_rate_limits_nm_per_s.min,
          hard.motor_torque_rise_rate_limits_nm_per_s.max),
    };
  }

  function controlProfileFromVehicleDefaults(actuationValue, hardLimitValue) {
    if (!actuationValue || typeof actuationValue !== 'object') {
      throw new TypeError('control actuation defaults must be an object');
    }
    const hard = normalizeVehicleHardLimits(hardLimitValue);
    return mergeControlProfileWithHardLimits({
      profile_version: CONTROL_PROFILE_VERSION,
      parking_idle_timeout_ms: 500,
      consecutive_late_frame_limit: 3,
      critical_camera_stale_brake_ms: hard.default_critical_camera_stale_brake_ms,
      control_recovery_timeout_ms: hard.default_control_recovery_timeout_ms,
      target_speed_kph: hard.max_target_speed_kph / 2,
      max_motor_torque_nm: hard.full_scale_motor_torque_nm / 2,
      max_brake_pressure_bar: hard.max_brake_pressure_bar / 2,
      service_brake_pressure_bar: hard.max_brake_pressure_bar / 2,
      hard_brake_pressure_bar: hard.max_brake_pressure_bar / 2,
      max_steering_angle_deg: hard.max_steering_angle_deg,
      speed_pid_kp: hard.default_speed_pid_kp,
      speed_pid_ki: hard.default_speed_pid_ki,
      speed_pid_kd: hard.default_speed_pid_kd,
      speed_pid_derivative_filter_tau_ms:
          hard.default_speed_pid_derivative_filter_tau_ms,
      speed_pid_max_dt_ms: hard.default_speed_pid_max_dt_ms,
      motor_torque_rise_rate_nm_per_s:
          hard.default_motor_torque_rise_rate_nm_per_s,
    }, hard);
  }

  function controlProfileThrottleLimit(profileValue, hardLimitValue) {
    const profile = normalizeControlProfile(profileValue);
    const hard = normalizeVehicleHardLimits(hardLimitValue);
    if (hard.max_speed_kph === 0) return 0;
    return clamp(profile.target_speed_kph / hard.max_speed_kph, 0, hard.max_throttle);
  }

  function degradedInputRearmRequired(timing, lastNativeAcceptedControlSeq) {
    if (timing && timing.rearm_required === false) return false;
    const seq = timing && timing.last_valid_command_seq;
    if (Number.isSafeInteger(seq) && seq > 0 &&
        Number.isSafeInteger(lastNativeAcceptedControlSeq) && lastNativeAcceptedControlSeq > seq) return false;
    return true;
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
    return CONTROL_PROFILE_FIELDS.every(field => left[field] === right[field]);
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

  function blockAndClearKeys(pressedCodes, blockedCodes, preserveBrake = false) {
    requireKeySet(pressedCodes, 'pressedCodes');
    requireKeySet(blockedCodes, 'blockedCodes');
    for (const code of pressedCodes) {
      const action = keyAction(code);
      if (preserveBrake && (action === 'service_brake' || action === 'hard_brake')) continue;
      blockedCodes.add(code);
      pressedCodes.delete(code);
    }
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

  function createGearChangeStationaryEvidence() {
    return {
      firstObservedAtMs: 0,
      lastObservedAtMs: 0,
      lastStatusSeq: 0,
      sampleCount: 0,
      confirmed: false,
    };
  }

  function updateGearChangeStationaryEvidence(
      previousValue, vcuStatus, statusSequenceValue, observedAtMsValue) {
    const previous = previousValue && typeof previousValue === 'object'
      ? previousValue : createGearChangeStationaryEvidence();
    const statusSequence = Number(statusSequenceValue);
    const observedAtMs = Number(observedAtMsValue);
    if (!Number.isSafeInteger(statusSequence) || statusSequence <= 0 ||
        !Number.isFinite(observedAtMs) || observedAtMs < 0) {
      return createGearChangeStationaryEvidence();
    }
    if (statusSequence <= Number(previous.lastStatusSeq || 0)) return previous;

    const speed = Number(vcuStatus && vcuStatus.speed_mps);
    if (!vcuStatus || vcuStatus.speed_valid !== true || !Number.isFinite(speed) ||
        Math.abs(speed) > GEAR_CHANGE_STATIONARY_SPEED_MPS) {
      return {
        ...createGearChangeStationaryEvidence(),
        lastStatusSeq: statusSequence,
      };
    }

    const continuing = Number(previous.sampleCount || 0) > 0 &&
        observedAtMs >= Number(previous.lastObservedAtMs || 0);
    const firstObservedAtMs = continuing
      ? Number(previous.firstObservedAtMs) : observedAtMs;
    const sampleCount = continuing ? Number(previous.sampleCount) + 1 : 1;
    return {
      firstObservedAtMs,
      lastObservedAtMs: observedAtMs,
      lastStatusSeq: statusSequence,
      sampleCount,
      confirmed: sampleCount >= GEAR_CHANGE_STATIONARY_MIN_SAMPLES &&
          observedAtMs - firstObservedAtMs >= GEAR_CHANGE_STATIONARY_MIN_DURATION_MS,
    };
  }

  function allowsGearChange(selectedGear, requestedGear, vcuStatus) {
    if (selectedGear === requestedGear || mockUnsupported(vcuStatus)) return true;
    const speed = Number(vcuStatus && vcuStatus.speed_mps);
    return Boolean(vcuStatus && vcuStatus.speed_valid) &&
        vcuStatus.gear_change_stationary_confirmed === true &&
        Number.isFinite(speed) &&
        Math.abs(speed) <= GEAR_CHANGE_STATIONARY_SPEED_MPS;
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

  function createGearTransition(fromGearValue, toGearValue, statusFloorValue, generationValue) {
    const fromGear = String(fromGearValue || '');
    const toGear = String(toGearValue || '');
    const statusFloor = Number(statusFloorValue);
    const generation = Number(generationValue);
    if (!['N', 'R', 'D'].includes(fromGear) || !['N', 'R', 'D'].includes(toGear) ||
        fromGear === toGear) {
      throw new TypeError('gear transition requires distinct N/R/D gears');
    }
    if (!Number.isSafeInteger(statusFloor) || statusFloor < 0) {
      throw new TypeError('gear transition status floor must be a non-negative safe integer');
    }
    if (!Number.isSafeInteger(generation) || generation <= 0) {
      throw new TypeError('gear transition generation must be a positive safe integer');
    }
    return {generation, fromGear, toGear, statusFloor, forwardedSeqs: []};
  }

  function recordForwardedGearCommand(
      transition, generationValue, commandSequenceValue, commandGearValue) {
    if (!transition || Number(generationValue) !== transition.generation ||
        String(commandGearValue || '') !== transition.toGear) {
      return transition;
    }
    const sequence = Number(commandSequenceValue);
    if (!Number.isSafeInteger(sequence) || sequence <= 0) return transition;
    const prior = Array.isArray(transition.forwardedSeqs) ? transition.forwardedSeqs : [];
    if (prior.includes(sequence)) return transition;
    return {...transition, forwardedSeqs: [...prior, sequence].slice(-64)};
  }

  function matchesGearChangeRejection(transition, message, selectedGearValue) {
    if (!transition || !message ||
        message.issue_code !== 'vcu_drive_gear_change_moving_or_stale' ||
        String(selectedGearValue || '') !== transition.toGear) {
      return false;
    }
    const intentSequence = Number(message.intent_seq);
    const commandSequence = Number.isSafeInteger(intentSequence) && intentSequence > 0
      ? intentSequence : Number(message.command_seq);
    const statusSequence = Number(message.control_status_seq);
    return Number.isSafeInteger(commandSequence) && commandSequence > 0 &&
        Number.isSafeInteger(statusSequence) && statusSequence > transition.statusFloor &&
        Array.isArray(transition.forwardedSeqs) &&
        transition.forwardedSeqs.includes(commandSequence);
  }

  function reduceGearChangeRejection(transition, message, selectedGearValue) {
    const selectedGear = ['N', 'R', 'D'].includes(String(selectedGearValue || ''))
      ? String(selectedGearValue) : 'N';
    const matched = matchesGearChangeRejection(transition, message, selectedGear);
    if (!matched) {
      return {
        matched: false,
        selectedGear,
        pendingGearRequest: null,
        pendingGearTransition: null,
        inhibitOrdinaryControl: true,
        sendRollback: false,
      };
    }
    return {
      matched: true,
      selectedGear: transition.fromGear,
      pendingGearRequest: transition.toGear,
      pendingGearTransition: null,
      inhibitOrdinaryControl: false,
      sendRollback: true,
    };
  }

  function telemetryConfirmsGearTransition(transition, message) {
    if (!transition || !message || message.event !== 'vehicle_telemetry') return false;
    if (!Array.isArray(transition.forwardedSeqs) || !transition.forwardedSeqs.length) {
      return false;
    }
    const statusSequence = Number(message.control_status_seq);
    if (!Number.isSafeInteger(statusSequence) || statusSequence <= transition.statusFloor) {
      return false;
    }
    const feedback = message.can_feedback;
    let actualGear = '';
    if (feedback && feedback.supported === true) {
      if (feedback.feedback_fresh !== true || feedback.gear_valid !== true) return false;
      actualGear = ({1: 'N', 2: 'R', 3: 'D', 4: 'P'})[Number(feedback.gear)] || '';
    } else if (feedback && feedback.supported === false) {
      actualGear = String(message.gear || '');
    } else {
      return false;
    }
    return actualGear === transition.toGear;
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
    preserveBrakeDuringRearm = false,
    steering = 0,
  }) {
    const sampledThrottle = clamp(finiteNumber(throttle), 0, 1);
    const sampledBrake = clamp(finiteNumber(brake), 0, 1);
    if (!authorityReady) return {requiresNeutral: true, throttle: 0, brake: 0};
    if (gearRequestPending) return {
      requiresNeutral: true, throttle: 0, brake: preserveBrakeDuringRearm ? sampledBrake : 0};
    if (requiresNeutral) {
      const pedalsNeutral = sampledThrottle === 0 && sampledBrake === 0 &&
          (!preserveBrakeDuringRearm || finiteNumber(steering) === 0);
      return {requiresNeutral: !pedalsNeutral, throttle: 0, brake: preserveBrakeDuringRearm ? sampledBrake : 0};
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

  function deriveEstopPresentation(
      localLatched,
      vehicleActive,
      stopSourceValue = '',
      stopReasonValue = '') {
    const local = Boolean(localLatched);
    const vehicle = Boolean(vehicleActive);
    const stopSource = String(stopSourceValue || '').trim();
    const stopReason = String(stopReasonValue || '').trim();
    const reasonSuffix = stopReason ? `（${stopReason}）` : '';
    // Physical emergency is authoritative even when a page ESTOP was already
    // latched; the bridge deliberately allows this source to supersede an
    // earlier software stop so the operator sees the higher-priority cause.
    if (vehicle && stopSource === 'physical_estop') {
      return {
        kind: 'physical_estop',
        visible: true,
        severity: 'critical',
        banner: `车辆物理急停已触发${reasonSuffix}；请立即核实现场并按现场流程复位。`,
        alert: `车辆物理急停已触发${reasonSuffix}；请立即核实现场`,
      };
    }
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
      if (stopSource === 'page_disconnect') {
        return {
          kind: 'page_disconnect',
          visible: true,
          severity: 'warn',
          banner: `车端已按页面断开 VCU 握手请求进入安全停车${reasonSuffix}；重新连接后请重新确认参数并申请握手。`,
          alert: `页面断开触发车端安全停车${reasonSuffix}；恢复驾驶前需要重新申请 VCU 握手`,
        };
      }
      if (stopSource === 'page_request') {
        return {
          kind: 'page_request',
          visible: true,
          severity: 'critical',
          banner: `车端已执行控制页面急停请求${reasonSuffix}；车辆必须本地确认后才能复位。`,
          alert: `控制页面请求的车辆急停已由车端确认${reasonSuffix}`,
        };
      }
      if (stopSource === 'session_loss') {
        return {
          kind: 'session_loss',
          visible: true,
          severity: 'critical',
          banner: `控制会话丢失，车端已主动安全停车${reasonSuffix}；请确认链路后重新连接。`,
          alert: `控制会话丢失触发车端安全停车${reasonSuffix}`,
        };
      }
      if (stopSource === 'watchdog') {
        return {
          kind: 'watchdog',
          visible: true,
          severity: 'critical',
          banner: `车端 watchdog 已触发安全停车${reasonSuffix}；请检查控制指令时序后重新连接。`,
          alert: `车端 watchdog 触发安全停车${reasonSuffix}`,
        };
      }
      if (stopSource === 'software_fault') {
        return {
          kind: 'software_fault',
          visible: true,
          severity: 'critical',
          banner: `车端软件故障已触发安全停车${reasonSuffix}；请先检查车端日志再复位。`,
          alert: `车端软件故障触发安全停车${reasonSuffix}`,
        };
      }
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

  function deriveControlStopPresentation(localEstop, vehicleEstop, stopSource, stopReason, handshake) {
    const safety = deriveEstopPresentation(localEstop, vehicleEstop, stopSource, stopReason);
    if (safety.visible && safety.kind !== 'software_fault') return safety;
    const diagnostic = vehicleControlDiagnostic(handshake && handshake.issue_code, handshake && handshake.control_recovery);
    if (diagnostic && diagnostic.level === 'critical') {
      return {kind: 'control_fault', visible: true, severity: 'critical',
        banner: diagnostic.text, alert: diagnostic.text};
    }
    return safety;
  }

  function deriveControlRecoveryPresentation(connected, telemetry, feedbackFresh, hardStopVisible) {
    const hidden = {visible: false, severity: '', banner: '', alert: '', cause: ''};
    if (!connected || !telemetry || hardStopVisible || telemetry.estop === true ||
        ['ESTOP', 'FAULT', 'TIMEOUT_BRAKE'].includes(telemetry.safety_state)) return hidden;
    const timing = telemetry.control_timing || {};
    const waiting = timing.control_braking_wait === true;
    const holding = timing.recovery_brake_hold === true;
    const delayed = telemetry.safety_state === 'DEGRADED';
    if (!waiting && !holding && !delayed) return hidden;
    const cause = waiting || holding ? String(timing.control_recovery_cause || '') : 'control_delay';
    const reasons = {critical_video: '关键视频中断或无新帧', time_sync: '时间同步不可信',
      input_lease: '驾驶输入刷新过期', control_link: '控制链路 ACK 确认暂时中断', control_delay: '控制指令延迟或短暂中断'};
    const reason = reasons[cause] || '控制链路暂时异常';
    const guidance = controlRecoveryGuidance(timing, feedbackFresh);
    // Report the last authoritative vehicle state without claiming that old
    // feedback confirms current physical braking or a completed stop.
    const banner = !feedbackFresh
      ? `车端此前已报告${waiting || holding ? '制动等待' : '控制暂缓'}（${reason}）；反馈已过期，当前控制状态待确认。`
      : guidance.visible
        ? `车端报告${waiting ? '制动等待' : '制动保持'}；当前会话保留。${guidance.text}`
          : `车端控制暂缓：${reason}，牵引已撤销；当前会话保留。请释放旧按键再重新按下；持续无有效控制将安全停车。`;
    return {visible: true, severity: 'warn', banner, alert: banner, cause};
  }

  function controlRecoveryGuidance(timingValue, feedbackFresh = true) {
    const timing = timingValue && typeof timingValue === 'object' ? timingValue : {};
    const waiting = timing.control_braking_wait === true;
    const holding = timing.recovery_brake_hold === true;
    const rearm = timing.rearm_required === true;
    const hidden = {visible: false, phase: 'none', causes: [], reason: '', nextStep: '', text: ''};
    if (!waiting && !holding && !rearm) return hidden;
    const labels = {critical_video: '关键视频未就绪', input_lease: '驾驶输入刷新未就绪',
      control_link: '控制链路 ACK 确认未就绪', time_sync: '时间同步不可信'};
    const causes = [];
    if (timing.critical_video_required !== false && timing.driver_critical_video_ready === false) causes.push('critical_video');
    if (timing.operator_input_ready === false) causes.push('input_lease');
    if (timing.driver_control_link_ready === false) causes.push('control_link');
    if (timing.driver_time_sync_ready === false) causes.push('time_sync');
    let phase, reason, nextStep;
    if (!feedbackFresh) {
      phase = 'feedback_unknown'; reason = '车端恢复反馈已过期，当前状态待确认';
      nextStep = '保持方向输入释放，等待新鲜车端反馈';
    } else if (causes.length || timing.recovery_samples_healthy !== true) {
      phase = 'wait_conditions'; reason = causes.length ? causes.map(cause => labels[cause]).join('、') :
        timing.recovery_samples_healthy === false ? '必需条件尚未连续健康' : '必需条件恢复尚未确认';
      if (!causes.length && timing.recovery_samples_healthy !== false && labels[timing.control_recovery_cause]) reason += `（上次：${labels[timing.control_recovery_cause]}）`;
      nextStep = '保持方向输入释放，等待条件恢复及连续健康确认';
    } else if (waiting) {
      phase = 'healthy_window'; reason = '必需条件正在连续健康确认';
      nextStep = '保持方向输入释放，等待 300 ms 连续健康确认后再重新操作';
    } else {
      phase = 'await_fresh_input'; reason = '等待新的驾驶操作，制动保持';
      nextStep = '释放旧方向输入、归零后重新按下；持续制动可保持';
    }
    return {visible: true, phase, causes, reason, nextStep, text: `当前阻挡：${reason} · 下一步：${nextStep}`};
  }

  function deriveControlCommandRejection(issueCodeValue) {
    const issueCode = String(issueCodeValue || '');
    if (issueCode === 'control_input_rearm_required') {
      return {
        issueCode,
        action: 'rearm_input',
        clearInput: true,
        severity: 'warn',
        text: '控制命令短暂中断，输入已清除并保持当前挡位；请释放控制键后重新按下。',
      };
    }
    if (issueCode === 'vcu_drive_gear_change_moving_or_stale') {
      return {
        issueCode,
        action: 'rollback_gear_change',
        clearInput: true,
        severity: 'warn',
        text: '换挡被车端拒绝：已撤销牵引并保持拒绝前挡位；请停车、恢复新鲜反馈，释放后再重新选择方向。',
      };
    }
    return {
      issueCode: 'vcu_control_apply_rejected',
      action: 'reset_neutral',
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

  function controlPrepareDeadlineMs(maxCommandGapMs) {
    const gap = Number(maxCommandGapMs);
    return Number.isFinite(gap) && gap > 0
      ? Math.max(1, Math.min(100, Math.floor(gap / 2)))
      : 100;
  }

  function createLatestControlWriteQueue(writeControl, onUnhandledError, onPreemptWrite) {
    if (typeof writeControl !== 'function') throw new TypeError('writeControl must be a function');
    const reportUnhandled = typeof onUnhandledError === 'function'
      ? onUnhandledError
      : function noop() {};
    const notifyPreempt = typeof onPreemptWrite === 'function'
      ? onPreemptWrite
      : function noop() {};
    const active = new Set();
    let pending = null;

    function mergeExtra(current, incoming) {
      return Object.assign({}, current, incoming, {
        estop: Boolean((current && current.estop) || (incoming && incoming.estop)),
      });
    }

    function preempt(request, reason) {
      if ((request.extra.estop && reason !== 'session_changed') || request.preempted) return;
      request.preempted = true;
      notifyPreempt(request, reason);
    }

    function drain() {
      if (!pending) return;
      const urgent = Boolean(pending.extra.estop);
      if (urgent) {
        // Reserve a third slot for ESTOP; ordinary responses never consume it.
        for (const request of active) preempt(request, 'estop');
      }
      const limit = urgent ? 3 : 2;
      if (active.size >= limit) {
        // A heartbeat waits in the single latest slot for an existing response
        // deadline. Cancelling at the third 66ms tick would starve otherwise
        // valid 132-150ms replies, including gear-transition confirmations.
        if (pending.waiters.length) {
          const oldest = Array.from(active).find(request => !request.extra.estop && !request.preempted);
          if (oldest) preempt(oldest, urgent ? 'estop' : 'latest_input');
        }
        return;
      }
      const request = pending;
      pending = null;
      active.add(request);
      let result;
      try { result = writeControl(request.extra, request.announceUnavailable, request); }
      catch (error) { result = Promise.reject(error); }
      Promise.resolve(result).then(value => {
        for (const waiter of request.waiters) waiter.resolve(value);
      }, error => {
        if (!request.waiters.length) reportUnhandled(error);
        for (const waiter of request.waiters) waiter.reject(error);
      }).finally(() => {
        active.delete(request);
        drain();
      }).catch(reportUnhandled);
    }

    function send(extra, announceUnavailable) {
      return new Promise(function enqueue(resolve, reject) {
        if (pending) {
          pending.extra = mergeExtra(pending.extra, extra);
          pending.announceUnavailable = pending.announceUnavailable || announceUnavailable;
          pending.waiters.push({resolve, reject});
        } else {
          pending = {
            extra: Object.assign({}, extra),
            announceUnavailable: Boolean(announceUnavailable),
            waiters: [{resolve, reject}],
          };
        }
        drain();
      });
    }

    function enqueueHeartbeat() {
      if (pending) return false;
      pending = {extra: {}, announceUnavailable: false, waiters: []};
      drain();
      return true;
    }

    function reset() {
      if (pending) {
        for (const waiter of pending.waiters) waiter.resolve({sent: false, reason: 'control_queue_session_changed'});
        pending = null;
      }
      for (const request of active) {
        // A preempted request may still be settling; revoke all old slots.
        notifyPreempt(request, 'session_changed');
      }
      active.clear();
    }

    return Object.freeze({send, enqueueHeartbeat, reset});
  }

  function iceOfferScope(offer, allowLegacyBootstrap) {
    const id = typeof offer.media_negotiation_id === 'string' && offer.media_negotiation_id.length <= 128
      ? offer.media_negotiation_id : '';
    const ufrags = [];
    const expression = /(?:^|\r?\n)a=ice-ufrag:([^\s\r\n]+)/g;
    for (let match; (match = expression.exec(String(offer.sdp || '')));) {
      if (match[1].length <= 256 && !ufrags.includes(match[1]) && ufrags.length < 128) ufrags.push(match[1]);
    }
    return {id, ufrags, allowLegacyBootstrap: Boolean(allowLegacyBootstrap) && offer.legacy_bootstrap_allowed !== false && !id};
  }

  function iceCandidateScope(candidate) {
    const id = typeof candidate.media_negotiation_id === 'string' && candidate.media_negotiation_id.length <= 128
      ? candidate.media_negotiation_id : '';
    const match = String(candidate.candidate || '').match(/(?:^|\s)ufrag\s+([^\s]+)/);
    const ufrag = typeof candidate.usernameFragment === 'string' ? candidate.usernameFragment : match ? match[1] : '';
    return {id, ufrag: ufrag.length <= 256 ? ufrag : ''};
  }

  function iceCandidateMatchesOffer(candidate, offer) {
    if (!offer) return false;
    const scope = iceCandidateScope(candidate);
    if (scope.id) {
      if (!offer.id || scope.id !== offer.id) return false;
      return !scope.ufrag || offer.ufrags.includes(scope.ufrag);
    }
    if (scope.ufrag) return offer.ufrags.includes(scope.ufrag);
    return offer.allowLegacyBootstrap === true;
  }

  function mediaScopeDiagnostic(issue) {
    if (!['media_negotiation_scope_missing', 'media_ice_scope_missing'].includes(issue)) return null;
    return {level: 'warn', text: '媒体信令版本不兼容，重试缺少协商标识。请保持车辆停止，统一更新控制端和车端包，重新打开控制页面并建立新会话。'};
  }

  function iceServerError(error) {
    const url = String(error.url || ''), message = String(error.errorText || '');
    const explicit = url.match(/[?&]transport=(udp|tcp)(?:&|$)/i);
    return {server_transport: explicit ? explicit[1].toLowerCase()
      : /^(turns|stuns):/i.test(url) ? 'tcp' : 'udp',
      error_category: /host lookup|name resolution|dns|resolve/i.test(message) ? 'dns_resolution'
        : /timed out|timeout/i.test(message) ? 'server_timeout' : 'unclassified'};
  }

  function iceConnectivity(report, selectedPair) {
    const candidates = {local: {}, remote: {}};
    const states = {}, pairs = [];
    for (const value of report.values()) {
      const side = value.type === 'local-candidate' ? 'local'
        : value.type === 'remote-candidate' ? 'remote' : null;
      if (side) {
        const key = String(value.candidateType || 'unknown') + '/' + String(value.protocol || 'unknown');
        candidates[side][key] = (candidates[side][key] || 0) + 1;
      }
      if (value.type !== 'candidate-pair') continue;
      const local = report.get(value.localCandidateId) || {}, remote = report.get(value.remoteCandidateId) || {};
      const state = value.state || 'unknown'; states[state] = (states[state] || 0) + 1;
      pairs.push({id: value.id, selected: Boolean(selectedPair && selectedPair.id === value.id),
        state, nominated: value.nominated === true,
        local_type: local.candidateType || null, remote_type: remote.candidateType || null,
        protocol: local.protocol || null, relay_protocol: local.relayProtocol || null,
        requests_sent: value.requestsSent == null ? null : value.requestsSent,
        responses_received: value.responsesReceived == null ? null : value.responsesReceived,
        round_trip_ms: Number.isFinite(value.currentRoundTripTime) ? value.currentRoundTripTime * 1000 : null});
    }
    // Bound logs; retain the selected pair and failing direct checks ahead of
    // unused relay permutations. No candidate IPs, SDP or credentials.
    const rank = pair => pair.selected ? 0
      : pair.local_type && pair.remote_type && pair.local_type !== 'relay' && pair.remote_type !== 'relay' ? 1 : 2;
    pairs.sort((a, b) => rank(a) - rank(b));
    return {candidates, pair_states: states, pair_count: pairs.length,
      pairs: pairs.slice(0, 16), pairs_truncated: pairs.length > 16};
  }

  function receiverLatency(stats, captureEncodeMs, roundTripSeconds) {
    function average(total, count) {
      return Number.isFinite(total) && total >= 0 && Number.isFinite(count) && count > 0
        ? total * 1000 / count : null;
    }
    const processingMs = average(stats.totalProcessingDelay, stats.framesDecoded);
    const jitterMs = average(stats.jitterBufferDelay, stats.jitterBufferEmittedCount);
    const decodeMs = average(stats.totalDecodeTime, stats.framesDecoded);
    // First RTP arrival -> decode already covers jitter/assembly waiting.
    // Older browsers can supply the narrower buffer + decode approximation.
    const receiverMs = processingMs !== null ? processingMs
      : jitterMs !== null && decodeMs !== null ? jitterMs + decodeMs : null;
    const valid = value => Number.isFinite(value) && value >= 0;
    return {processingMs, jitterMs, decodeMs, receiverMs,
      method: processingMs !== null ? 'rtp-arrival-to-decode'
        : receiverMs !== null ? 'jitter-buffer-plus-decode' : 'unavailable',
      latencyMs: valid(captureEncodeMs) && valid(roundTripSeconds) && receiverMs !== null
        ? captureEncodeMs + roundTripSeconds * 500 + receiverMs : null};
  }

  const CONTROL_OUTCOMES = Object.freeze([
    'forwarded',
    'superseded',
    'expired_before_forward',
    'post_prepare_link_changed',
    'post_prepare_vcu_not_ready',
  ]);

  function shouldLogControlOutcome(outcome) {
    // High-rate normal and expiry outcomes stay in the 1 Hz aggregate. The
    // page emits a separately throttled diagnostic for preparation expiry.
    if (outcome === 'prepared' || outcome === 'forwarded' ||
        outcome === 'superseded' || outcome === 'expired_before_forward') return false;
    if (!CONTROL_OUTCOMES.includes(outcome)) throw new TypeError('unknown control outcome');
    return true;
  }

  function createControlOutcomeMetrics() {
    return {
      prepared: 0,
      forwarded: 0,
      superseded: 0,
      expired_before_forward: 0,
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

  function telemetryFreshness(message, receivedAtMonotonicMs, nowMonotonicMs, synchronizedNowMs) {
    const receivedAge = message && receivedAtMonotonicMs != null &&
        Number.isFinite(receivedAtMonotonicMs) && Number.isFinite(nowMonotonicMs)
        ? Math.max(0, nowMonotonicMs - receivedAtMonotonicMs) : null;
    const sourceAge = message && message.sent_at_utc_ms != null && synchronizedNowMs != null &&
        Number.isFinite(Number(message.sent_at_utc_ms)) && Number.isFinite(synchronizedNowMs)
        ? Math.max(0, synchronizedNowMs - Number(message.sent_at_utc_ms)) : null;
    return {
      received_age_ms: receivedAge,
      source_age_ms: sourceAge,
      fresh: receivedAge !== null && receivedAge <= 500 && sourceAge !== null && sourceAge <= 500,
    };
  }

  function recordingPresentation(status, scope, receivedAtMonotonicMs, nowMonotonicMs, synchronizedNowMs, previous = {}) {
    const scopeKey = scope && scope.session_id ? `${scope.vehicle_id}:${scope.session_id}` : '';
    if (!scopeKey) return {scopeKey, incomplete: false, text: '未连接', level: '', alerts: []};
    const matches = scope && typeof scope.vehicle_id === 'string' && scope.vehicle_id && typeof scope.session_id === 'string' && scope.session_id &&
        status && status.session_id === scope.session_id && status.vehicle_id === scope.vehicle_id;
    const lanes = matches && Array.isArray(status.lanes) ? status.lanes : [];
    const gap = previous.scopeKey === scopeKey && previous.incomplete === true ||
        lanes.some(lane => lane && typeof lane === 'object' && typeof lane.camera_id === 'string' && lane.camera_id &&
          lane.recording && lane.recording.schema_version === 1 && typeof lane.recording.incomplete === 'boolean' &&
          (lane.recording.incomplete === true || lane.recording.state === 'failed'));
    // The producer also sets incomplete for lost finalization evidence. Only
    // a known AU/storage loss cause proves a gap; do not turn unknown into loss.
    const knownLoss = record => ['recording_admission_overflow', 'recording_invalid_access_unit', 'recording_disk_full'].includes(record.issue_code);
    const gapConfirmed = previous.scopeKey === scopeKey && previous.gapConfirmed === true ||
        lanes.some(lane => lane && typeof lane.camera_id === 'string' && lane.camera_id && lane.recording &&
          lane.recording.schema_version === 1 && typeof lane.recording.incomplete === 'boolean' &&
          (lane.recording.incomplete === true || lane.recording.state === 'failed') && knownLoss(lane.recording));
    const idleAction = '车辆空闲后检查存储空间、目录权限和车端录制日志';
    const unknown = () => ({scopeKey, incomplete: gap, gapConfirmed, text: gapConfirmed ? '录像不完整，当前状态未确认' : gap ? '录像证据不足，完整性待确认' : '录像状态未确认',
      level: 'warn', alerts: [`${gapConfirmed ? '本会话录像存在已确认缺口；' : gap ? '录像证据不足，完整性待确认；' : ''}录像状态未确认。下一步：${idleAction}；保留已有片段和日志，旧版车端请更新以取得录制反馈。`]});
    // Media status normally arrives once per second. This three-second bound
    // controls diagnostic presentation only, never the critical-video gate.
    if (!matches || !lanes.length || lanes.length > 64 || !Number.isFinite(receivedAtMonotonicMs) ||
        !Number.isFinite(nowMonotonicMs) || nowMonotonicMs < receivedAtMonotonicMs || nowMonotonicMs - receivedAtMonotonicMs > 3000 ||
        !Number.isFinite(status.reported_at_utc_ms) || !Number.isFinite(synchronizedNowMs) ||
        status.reported_at_utc_ms > synchronizedNowMs || synchronizedNowMs - status.reported_at_utc_ms > 3000) return unknown();
    const states = ['disabled', 'starting', 'recording', 'failed', 'finalizing', 'finalized', 'cleanup_pending'];
    const number = value => Number.isSafeInteger(value) && value >= 0;
    if (lanes.some(lane => !lane || typeof lane !== 'object' || typeof lane.camera_id !== 'string' || !lane.camera_id || !lane.recording ||
        lane.recording.schema_version !== 1 || !states.includes(lane.recording.state) ||
        typeof lane.recording.recording_attempt_id !== 'string' || lane.recording.configured_enabled === true && !lane.recording.recording_attempt_id ||
        lane.recording.configured_enabled === false && (lane.recording.active !== false || lane.recording.state !== 'disabled') ||
        ['configured_enabled', 'active', 'incomplete', 'final_eos_received', 'cleanup_pending', 'summary_persisted']
          .some(key => typeof lane.recording[key] !== 'boolean') ||
        ['accepted_access_units', 'worker_input_access_units', 'finalized_segments', 'unfinished_segments', 'status_revision', 'summary_revision']
          .some(key => !number(lane.recording[key]))) || new Set(lanes.map(lane => lane.camera_id)).size !== lanes.length) return unknown();
    const enabled = lanes.filter(lane => lane.recording.configured_enabled === true);
    // A fresh parent poll can carry old child status. Use the worker's source
    // timestamp too; receiving the same status again cannot renew its evidence.
    if (enabled.some(lane => !number(lane.recording.reported_at_utc_ms) || lane.recording.reported_at_utc_ms === 0 ||
        lane.recording.reported_at_utc_ms > synchronizedNowMs || synchronizedNowMs - lane.recording.reported_at_utc_ms > 3000)) return unknown();
    const action = lane => typeof lane.recording.next_action === 'string' && lane.recording.next_action.trim()
      ? lane.recording.next_action.trim().slice(0, 512) : idleAction;
    if (gap) {
      const faults = enabled.filter(lane => lane.recording.incomplete || lane.recording.state === 'failed' || lane.recording.cleanup_pending);
      const alerts = faults.length ? faults.map(lane => `${lane.camera_id}：${knownLoss(lane.recording) ? '录像不完整，已确认缺口，不能补回此前缺录' : '录像证据不足，完整性待确认'}${lane.recording.cleanup_pending ? '，收尾未完成' : ''}。下一步：${action(lane)}。`) : [];
      if (gapConfirmed) alerts.push(`本会话录像存在已确认缺口；当前录制恢复也不能补回此前缺口。下一步：${idleAction}，保存现有日志和录像供核查。`);
      else if (!faults.length) alerts.push(`此前录像完整性尚未获确认；当前录制恢复不能证明此前完整。下一步：${idleAction}，保留已有片段和日志核查封装证据。`);
      return {scopeKey, incomplete: true, gapConfirmed, text: gapConfirmed ? '录像不完整' : '录像证据不足，完整性待确认', level: 'warn', alerts};
    }
    if (!enabled.length) return {scopeKey, incomplete: false, text: '未启用', level: '', alerts: []};
    if (enabled.every(lane => {const value=lane.recording;return value.state==='finalized' && !value.active && value.final_eos_received &&
        !value.cleanup_pending && value.summary_persisted && value.accepted_access_units>0 &&
        value.worker_input_access_units===value.accepted_access_units && value.finalized_segments>0 && value.unfinished_segments===0 && value.status_revision>0 && value.summary_revision>0}))
      return {scopeKey, incomplete: false, text: '车端报告本次录像封装完成', level: '', alerts: []};
    if (enabled.some(lane => lane.recording.cleanup_pending)) return {scopeKey, incomplete: false, text: '录像收尾未完成', level: 'warn',
      alerts: [`录像收尾仍待确认。下一步：${idleAction}，等待车端完成清理；保留已有片段。`]};
    if (enabled.some(lane => lane.recording.state==='finalizing')) return {scopeKey, incomplete: false, text: '录像封装中', level: 'warn', alerts: []};
    if (enabled.every(lane => lane.recording.state==='recording' && lane.recording.active && lane.recording.worker_input_access_units>0))
      return {scopeKey, incomplete: false, text: '录制进行中，完整性待结束确认', level: '', alerts: []};
    if (enabled.some(lane => lane.recording.state==='starting')) return {scopeKey, incomplete: false, text: '录像启动中', level: 'warn', alerts: []};
    return unknown();
  }

  function safeLogoutFeedback(request, message, receivedAtMonotonicMs, nowMonotonicMs, synchronizedNowMs) {
    const rejected = reason => ({confirmed: false, reason});
    if (!request || !message || message.event !== 'vehicle_telemetry') return rejected('feedback_missing');
    if (message.session_id !== request.session_id || message.vehicle_id !== request.vehicle_id || message.driver_id !== request.driver_id) return rejected('session_changed');
    if (!Number.isSafeInteger(message.control_status_seq) || message.control_status_seq <= request.status_floor ||
        !Number.isFinite(receivedAtMonotonicMs) || receivedAtMonotonicMs < request.started_at_monotonic_ms) return rejected('feedback_not_new');
    if (!Number.isFinite(synchronizedNowMs) || !Number.isFinite(request.source_floor_utc_ms)) return rejected('clock_untrusted');
    const sentAt = message.sent_at_utc_ms;
    if (!Number.isFinite(sentAt) || sentAt < request.source_floor_utc_ms || sentAt > synchronizedNowMs ||
        !telemetryFreshness(message, receivedAtMonotonicMs, nowMonotonicMs, synchronizedNowMs).fresh) return rejected('feedback_stale');
    const adapter = message.vehicle_adapter || {}, handshake = message.vcu_handshake || {}, feedback = message.can_feedback || {};
    if (adapter.opened !== true || adapter.healthy !== true || adapter.last_error || handshake.state === 'fault' || handshake.state === 'closed') return rejected('adapter_failed');
    // The current bridge exposes Disarmed only after the final safe CAN batch
    // succeeded. A local command/HTTP response or a cached handshake is not evidence.
    if (handshake.supported !== true || handshake.state !== 'disarmed' || handshake.disarming !== false ||
        handshake.ready !== false || handshake.requested !== false) return rejected('disarm_pending');
    const parked = Array.isArray(feedback.parking_brake_valid) && feedback.parking_brake_valid.length === 4 &&
        feedback.parking_brake_valid.every(value => value === true) && Array.isArray(feedback.parking_brake_status) &&
        feedback.parking_brake_status.length === 4 && feedback.parking_brake_status.every(value => value === 2);
    // Keep the bridge's existing 0.1 m/s stopped tolerance; this is protocol
    // evidence, not a new chassis threshold or proof of mechanical parking.
    if (feedback.supported !== true || feedback.feedback_fresh !== true || !Number.isFinite(feedback.max_feedback_age_ms) ||
        feedback.max_feedback_age_ms < 0 || feedback.max_feedback_age_ms > 500 || feedback.speed_valid !== true ||
        !Number.isFinite(feedback.speed_mps) || Math.abs(feedback.speed_mps) > 0.1 || feedback.gear_valid !== true ||
        feedback.gear !== 1 || feedback.handshake_valid !== true || feedback.handshake_status !== 3 || !parked) return rejected('can_feedback_unconfirmed');
    return {confirmed: true, reason: 'disarm_confirmed', control_status_seq: message.control_status_seq, estop_confirmed: message.estop === true};
  }

  function safeLogoutPresentation(result, released, estopLatched) {
    const labels = {feedback_missing: '尚无车端反馈', feedback_not_new: '未收到退出请求后的新反馈',
      feedback_stale: '车端反馈已过期', clock_untrusted: '反馈源时间不可信', adapter_failed: '车端适配器或 CAN 故障',
      disarm_pending: '车端退出尚未完成', can_feedback_unconfirmed: '完整 CAN 退出反馈未确认',
      timeout: '等待车端确认超时', link_lost: '反馈链路中断', session_changed: '会话或操作页面已变化',
      unsupported: '当前适配器没有可验证的 VCU 退出反馈', no_active_session: '没有活动车端会话',
      request_failed: '退出请求未确认', release_failed: '控制会话释放未确认'};
    const confirmed = result && result.confirmed === true && released === true;
    const reason = labels[result && result.reason] || '车端安全状态未确认';
    let text = confirmed
      ? '车端退出协议及最终 CAN 写入已确认；控制会话已释放。下一步：在车辆本地核实停车状态。'
      : `${released ? '控制会话已释放；' : ''}安全退出未确认（${reason}）。下一步：在车辆本地核实停车和制动状态；必要时使用物理急停。`;
    if (estopLatched) text += result && result.estop_confirmed === true
      ? ' 此次退出期间车端已报告急停；急停请求仍保持锁存，须本地确认后复位。'
      : ' 急停请求仍保持锁存；请在车辆本地核实，须本地确认后复位。';
    return {confirmed, text};
  }

  function selectedIcePair(report) {
    for (const stat of report.values()) {
      if (stat.type !== 'transport' || !stat.selectedCandidatePairId) continue;
      const pair = report.get(stat.selectedCandidatePairId);
      if (pair && pair.type === 'candidate-pair') return {pair, transport: stat, method: 'transport'};
    }
    // Older browsers may omit transport.selectedCandidatePairId. A successful
    // connectivity check alone does not identify the pair carrying media.
    for (const pair of report.values()) {
      if (pair.type === 'candidate-pair' && pair.state === 'succeeded' && pair.nominated === true) {
        return {pair, transport: null, method: 'nominated_fallback'};
      }
    }
    return {pair: null, transport: null, method: 'unavailable'};
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
    GEAR_CHANGE_STATIONARY_SPEED_MPS,
    GEAR_CHANGE_STATIONARY_MIN_SAMPLES,
    GEAR_CHANGE_STATIONARY_MIN_DURATION_MS,
    createGearChangeStationaryEvidence,
    updateGearChangeStationaryEvidence,
    allowsGearChange,
    deriveGearSelection,
    createGearTransition,
    recordForwardedGearCommand,
    matchesGearChangeRejection,
    reduceGearChangeRejection,
    telemetryConfirmsGearTransition,
    deriveControl,
    normalizeControlProfile,
    normalizeVehicleHardLimits,
    vehicleControlDiagnostic,
    deriveControlStopPresentation,
    mergeControlProfileWithHardLimits,
    controlProfileFromVehicleDefaults,
    controlProfileThrottleLimit,
    controlProfilesEqual,
    degradedInputRearmRequired,
    reduceControlProfileStatus,
    reduceGamepadNeutralInterlock,
    reduceStatusSequence,
    deriveEstopPresentation,
    deriveControlRecoveryPresentation,
    controlRecoveryGuidance,
    deriveControlCommandRejection,
    isCurrentPeer,
    isCurrentControlChannel,
    controlSnapshot,
    controlIntentSuperseded,
    controlPrepareDeadlineMs,
    createLatestControlWriteQueue,
    receiverLatency,
    iceConnectivity,
    iceServerError,
    CONTROL_OUTCOMES,
    shouldLogControlOutcome,
    createControlOutcomeMetrics,
    reduceControlOutcome,
    controlOutcomesBalanced,
    telemetryFreshness,
    recordingPresentation,
    safeLogoutFeedback,
    safeLogoutPresentation,
    selectedIcePair,
    mediaScopeDiagnostic,
    iceOfferScope,
    iceCandidateScope,
    iceCandidateMatchesOffer,
  });
});
