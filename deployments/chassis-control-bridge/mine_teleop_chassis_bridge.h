#ifndef MINE_TELEOP_CHASSIS_BRIDGE_H
#define MINE_TELEOP_CHASSIS_BRIDGE_H

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define MINE_TELEOP_CHASSIS_DEFAULT_FULL_SCALE_MOTOR_TORQUE_NM 300.0
#define MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM 640.0
#define MINE_TELEOP_CHASSIS_MIN_DBC_MOTOR_TORQUE_NM -800.0
#define MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM 838.3
#define MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM 0.1
#define MINE_TELEOP_CHASSIS_DEFAULT_MOTOR_TORQUE_RISE_RATE_NM_PER_SECOND 0.0
#define MINE_TELEOP_CHASSIS_MAX_MOTOR_TORQUE_RISE_RATE_NM_PER_SECOND 32000.0
#define MINE_TELEOP_CHASSIS_DEFAULT_MAX_BRAKE_PRESSURE_BAR 100.0
#define MINE_TELEOP_CHASSIS_MAX_ORDINARY_BRAKE_PRESSURE_BAR 327.6
#define MINE_TELEOP_CHASSIS_MAX_EMERGENCY_BRAKE_PRESSURE_BAR 409.5
#define MINE_TELEOP_CHASSIS_BRAKE_PRESSURE_RESOLUTION_BAR 0.1
#define MINE_TELEOP_CHASSIS_DEFAULT_CONTROL_TIMEOUT_MS 800
#define MINE_TELEOP_CHASSIS_MIN_CONTROL_TIMEOUT_MS 20
#define MINE_TELEOP_CHASSIS_MAX_CONTROL_TIMEOUT_MS 60000
#define MINE_TELEOP_CHASSIS_MIN_SPEED_FEEDBACK_TIMEOUT_MS 20
#define MINE_TELEOP_CHASSIS_MAX_SPEED_FEEDBACK_TIMEOUT_MS 500
#define MINE_TELEOP_CHASSIS_MIN_SPEED_PID_MAX_DT_MS 20
#define MINE_TELEOP_CHASSIS_MAX_SPEED_PID_MAX_DT_MS 200
#define MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN 100.0
#define MINE_TELEOP_CHASSIS_MAX_DERIVATIVE_FILTER_TAU_MS 2000.0
#define MINE_TELEOP_CHASSIS_SPEED_PID_SETPOINT_RESET_DEADBAND_MPS 0.05
#define MINE_TELEOP_CHASSIS_MAX_HARD_OVERSPEED_MARGIN_MPS 10.0
#define MINE_TELEOP_CHASSIS_LEGACY_SESSION_CONTROL_PROFILE_VERSION 2U
#define MINE_TELEOP_CHASSIS_SESSION_CONTROL_PROFILE_VERSION 3U
#define MINE_TELEOP_CHASSIS_MAX_STEERING_REQUEST 1.0

static inline int mine_teleop_chassis_finite(double value) {
    return value == value && value >= -DBL_MAX && value <= DBL_MAX;
}

static inline double mine_teleop_chassis_bounded_motor_torque_nm(
    double normalized_pid_output,
    double max_motor_torque_nm) {
    if (!mine_teleop_chassis_finite(normalized_pid_output) ||
        !mine_teleop_chassis_finite(max_motor_torque_nm) ||
        !(normalized_pid_output > 0.0) || !(max_motor_torque_nm > 0.0)) {
        return 0.0;
    }
    const double normalized_traction =
        normalized_pid_output < 1.0 ? normalized_pid_output : 1.0;
    double authoritative_limit = max_motor_torque_nm;
    if (authoritative_limit > MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM) {
        authoritative_limit = MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM;
    }
    if (authoritative_limit > MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM) {
        authoritative_limit = MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM;
    }
    return normalized_traction * authoritative_limit;
}

/* Convert the normalized local speed-PID output directly to the requested
 * per-motor torque. The session limit is authoritative and the result is
 * quantized toward zero to the DBC's 0.1 Nm resolution. */
static inline double mine_teleop_chassis_motor_torque_limit_nm(
    double normalized_pid_output,
    double max_motor_torque_nm) {
    const double requested_limit = mine_teleop_chassis_bounded_motor_torque_nm(
        normalized_pid_output, max_motor_torque_nm);
    return floor(
        nextafter(
            requested_limit / MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM,
            DBL_MAX)) *
        MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM;
}

/* Limit only rising traction. Any decrease, including normal release, is
 * applied immediately so stale positive torque cannot survive a stop path. */
static inline double mine_teleop_chassis_rise_limited_motor_torque_nm(
    double previous_motor_torque_nm,
    double target_motor_torque_nm,
    double rise_rate_nm_per_second,
    double dt_seconds) {
    if (!mine_teleop_chassis_finite(previous_motor_torque_nm) ||
        !mine_teleop_chassis_finite(target_motor_torque_nm) ||
        previous_motor_torque_nm < 0.0 || target_motor_torque_nm <= 0.0) {
        return 0.0;
    }
    if (target_motor_torque_nm <= previous_motor_torque_nm) {
        return target_motor_torque_nm;
    }
    if (rise_rate_nm_per_second == 0.0) return target_motor_torque_nm;
    if (!mine_teleop_chassis_finite(rise_rate_nm_per_second) ||
        rise_rate_nm_per_second < 0.0 ||
        rise_rate_nm_per_second >
            MINE_TELEOP_CHASSIS_MAX_MOTOR_TORQUE_RISE_RATE_NM_PER_SECOND ||
        !mine_teleop_chassis_finite(dt_seconds) || dt_seconds <= 0.0) {
        return 0.0;
    }
    return fmin(
        target_motor_torque_nm,
        previous_motor_torque_nm +
            rise_rate_nm_per_second *
                dt_seconds);
}

static inline double mine_teleop_chassis_directional_motor_torque_nm(
    int target_gear,
    double motor_torque_magnitude_nm) {
    if (!mine_teleop_chassis_finite(motor_torque_magnitude_nm) ||
        motor_torque_magnitude_nm <= 0.0) {
        return 0.0;
    }
    const double quantized_magnitude = floor(nextafter(
        motor_torque_magnitude_nm /
            MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM,
        DBL_MAX)) * MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM;
    if (target_gear == 3) {
        return quantized_magnitude > MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM
            ? MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM
            : quantized_magnitude;
    }
    if (target_gear == 2) {
        const double requested = -quantized_magnitude;
        return requested < MINE_TELEOP_CHASSIS_MIN_DBC_MOTOR_TORQUE_NM
            ? MINE_TELEOP_CHASSIS_MIN_DBC_MOTOR_TORQUE_NM
            : requested;
    }
    return 0.0;
}

/* DBC pressure is Q0.1 bar. Quantize ordinary requests toward zero so the
 * emitted physical pressure can never exceed the accepted session request. */
static inline double mine_teleop_chassis_quantize_brake_pressure_bar(
    double brake_pressure_bar) {
    if (!(brake_pressure_bar > 0.0)) return 0.0;
    double bounded = brake_pressure_bar;
    if (bounded > MINE_TELEOP_CHASSIS_MAX_ORDINARY_BRAKE_PRESSURE_BAR) {
        bounded = MINE_TELEOP_CHASSIS_MAX_ORDINARY_BRAKE_PRESSURE_BAR;
    }
    return floor(nextafter(
               bounded / MINE_TELEOP_CHASSIS_BRAKE_PRESSURE_RESOLUTION_BAR,
               DBL_MAX)) *
        MINE_TELEOP_CHASSIS_BRAKE_PRESSURE_RESOLUTION_BAR;
}

/* Validate every numeric ChassisControl output before applying saturation.
 * std::min/std::max do not reject NaN and can turn it into a limit value. */
static inline int mine_teleop_chassis_control_output_is_finite(
    double wheel_torque,
    double wheel_speed,
    double steering_angle,
    double steering_speed,
    double brake_pressure) {
    return mine_teleop_chassis_finite(wheel_torque) &&
        mine_teleop_chassis_finite(wheel_speed) &&
        mine_teleop_chassis_finite(steering_angle) &&
        mine_teleop_chassis_finite(steering_speed) &&
        mine_teleop_chassis_finite(brake_pressure);
}

static inline int mine_teleop_chassis_control_timeout_is_valid(
    int control_timeout_ms) {
    return control_timeout_ms >= MINE_TELEOP_CHASSIS_MIN_CONTROL_TIMEOUT_MS &&
        control_timeout_ms <= MINE_TELEOP_CHASSIS_MAX_CONTROL_TIMEOUT_MS;
}

struct MineTeleopChassisSpeedPidConfig {
    double kp;
    double ki;
    double kd;
    double derivative_filter_tau_ms;
    int32_t max_dt_ms;
};

struct MineTeleopChassisSpeedPidState {
    double integral;
    double filtered_measurement_derivative;
    double previous_measurement;
    int initialized;
};

static inline int mine_teleop_chassis_speed_pid_config_is_valid(
    const struct MineTeleopChassisSpeedPidConfig* config) {
    return config != 0 &&
        mine_teleop_chassis_finite(config->kp) && config->kp > 0.0 &&
        config->kp <= MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN &&
        mine_teleop_chassis_finite(config->ki) && config->ki >= 0.0 &&
        config->ki <= MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN &&
        mine_teleop_chassis_finite(config->kd) && config->kd >= 0.0 &&
        config->kd <= MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN &&
        mine_teleop_chassis_finite(config->derivative_filter_tau_ms) &&
        config->derivative_filter_tau_ms >= 0.0 &&
        config->derivative_filter_tau_ms <=
            MINE_TELEOP_CHASSIS_MAX_DERIVATIVE_FILTER_TAU_MS &&
        config->max_dt_ms >= MINE_TELEOP_CHASSIS_MIN_SPEED_PID_MAX_DT_MS &&
        config->max_dt_ms <= MINE_TELEOP_CHASSIS_MAX_SPEED_PID_MAX_DT_MS;
}

static inline void mine_teleop_chassis_speed_pid_reset(
    struct MineTeleopChassisSpeedPidState* state) {
    if (state == 0) return;
    state->integral = 0.0;
    state->filtered_measurement_derivative = 0.0;
    state->previous_measurement = 0.0;
    state->initialized = 0;
}

/* Keep a fixed reference through small analog setpoint jitter so integral
 * state survives, but reset after cumulative drift crosses the deadband. */
static inline int mine_teleop_chassis_speed_pid_setpoint_requires_reset(
    int reference_valid,
    int reference_gear,
    int target_gear,
    double reference_speed_mps,
    double target_speed_mps) {
    return !reference_valid || reference_gear != target_gear ||
        !mine_teleop_chassis_finite(reference_speed_mps) ||
        !mine_teleop_chassis_finite(target_speed_mps) ||
        fabs(target_speed_mps - reference_speed_mps) >
            MINE_TELEOP_CHASSIS_SPEED_PID_SETPOINT_RESET_DEADBAND_MPS;
}

/* Pure local speed PID. Production uses normalized output up to 1.0, or the
 * lower actuator-reachable ceiling when rising-torque shaping is enabled; the
 * acknowledged per-motor session torque limit is applied afterwards.
 * Derivative-on-measurement avoids target-step kick; conditional integration
 * prevents windup while retaining a positive I term at target. */
static inline double mine_teleop_chassis_speed_pid_step(
    const struct MineTeleopChassisSpeedPidConfig* config,
    struct MineTeleopChassisSpeedPidState* state,
    double target_speed_mps,
    double measured_speed_along_gear_mps,
    double traction_ceiling,
    double dt_seconds) {
    if (!mine_teleop_chassis_speed_pid_config_is_valid(config) || state == 0 ||
        !mine_teleop_chassis_finite(state->integral) ||
        !mine_teleop_chassis_finite(state->filtered_measurement_derivative) ||
        !mine_teleop_chassis_finite(state->previous_measurement) ||
        (state->initialized != 0 && state->initialized != 1) ||
        !mine_teleop_chassis_finite(target_speed_mps) || target_speed_mps <= 0.0 ||
        !mine_teleop_chassis_finite(measured_speed_along_gear_mps) ||
        !mine_teleop_chassis_finite(traction_ceiling) || traction_ceiling <= 0.0 ||
        !mine_teleop_chassis_finite(dt_seconds) || dt_seconds <= 0.0 ||
        dt_seconds * 1000.0 > (double)config->max_dt_ms) {
        mine_teleop_chassis_speed_pid_reset(state);
        return 0.0;
    }

    if (traction_ceiling > 1.0) traction_ceiling = 1.0;
    const double error = target_speed_mps - measured_speed_along_gear_mps;
    double derivative = 0.0;
    if (state->initialized) {
        const double raw_derivative =
            (measured_speed_along_gear_mps - state->previous_measurement) /
            dt_seconds;
        const double tau_seconds = config->derivative_filter_tau_ms / 1000.0;
        const double alpha = tau_seconds <= 0.0
            ? 1.0
            : dt_seconds / (tau_seconds + dt_seconds);
        state->filtered_measurement_derivative +=
            alpha * (raw_derivative - state->filtered_measurement_derivative);
        derivative = state->filtered_measurement_derivative;
    } else {
        state->initialized = 1;
    }
    state->previous_measurement = measured_speed_along_gear_mps;
    if (state->integral < 0.0) state->integral = 0.0;
    if (state->integral > traction_ceiling) state->integral = traction_ceiling;

    const double unconstrained = config->kp * error + state->integral -
        config->kd * derivative;
    const int saturated_high = unconstrained >= traction_ceiling;
    const int saturated_low = unconstrained <= 0.0;
    if ((!saturated_high && !saturated_low) ||
        (saturated_high && error < 0.0) ||
        (saturated_low && error > 0.0)) {
        state->integral += config->ki * error * dt_seconds;
        if (state->integral < 0.0) state->integral = 0.0;
        if (state->integral > traction_ceiling) state->integral = traction_ceiling;
    }

    double output = config->kp * error + state->integral -
        config->kd * derivative;
    if (output < 0.0) output = 0.0;
    if (output > traction_ceiling) output = traction_ceiling;
    return output;
}

static inline int mine_teleop_chassis_hard_overspeed(
    double target_speed_mps,
    double measured_speed_along_gear_mps,
    double margin_mps) {
    return mine_teleop_chassis_finite(target_speed_mps) &&
        mine_teleop_chassis_finite(measured_speed_along_gear_mps) &&
        mine_teleop_chassis_finite(margin_mps) && target_speed_mps >= 0.0 &&
        margin_mps > 0.0 &&
        measured_speed_along_gear_mps > target_speed_mps + margin_mps;
}

static inline int mine_teleop_chassis_hard_overspeed_latch(
    int already_latched,
    double target_speed_mps,
    double measured_speed_mps,
    double margin_mps) {
    return already_latched || mine_teleop_chassis_hard_overspeed(
        target_speed_mps, measured_speed_mps, margin_mps);
}

static inline int mine_teleop_chassis_opposite_direction_motion(
    int target_gear,
    double signed_speed_mps) {
    if (!mine_teleop_chassis_finite(signed_speed_mps)) return 1;
    if (target_gear == 3) return signed_speed_mps < -0.1;
    if (target_gear == 2) return signed_speed_mps > 0.1;
    return 0;
}

static inline int mine_teleop_chassis_control_watchdog_expired(
    int active_control_state,
    int has_reference,
    int already_latched,
    uint64_t elapsed_ms,
    int control_timeout_ms) {
    return active_control_state && has_reference && !already_latched &&
        mine_teleop_chassis_control_timeout_is_valid(control_timeout_ms) &&
        elapsed_ms >= (uint64_t)control_timeout_ms;
}
#ifdef __cplusplus
extern "C" {
#endif

struct MineTeleopChassisTelemetry {
    double speed_mps;
    int gear;
    double steering_feedback;
    double throttle_feedback;
    double brake_feedback;
    int estop;
    uint32_t stop_source;
    uint32_t stop_reason;
    uint64_t stop_sequence;
};

/* Stable stop provenance carried from the caller into the bridge and returned
 * with telemetry. The first stop cause remains authoritative until the stop is
 * cleared; a subsequently asserted physical emergency may supersede it. */
enum MineTeleopChassisStopSource {
    MINE_TELEOP_CHASSIS_STOP_SOURCE_NONE = 0,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_DRIVER_PAGE = 1,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_SESSION = 2,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_WATCHDOG = 3,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_SOFTWARE_FAULT = 4,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_PHYSICAL_EMERGENCY = 5,
    MINE_TELEOP_CHASSIS_STOP_SOURCE_UNKNOWN = 255,
};

enum MineTeleopChassisStopReason {
    MINE_TELEOP_CHASSIS_STOP_REASON_NONE = 0,
    MINE_TELEOP_CHASSIS_STOP_REASON_OPERATOR_ESTOP = 1,
    MINE_TELEOP_CHASSIS_STOP_REASON_VCU_HANDSHAKE_DISCONNECT = 2,
    MINE_TELEOP_CHASSIS_STOP_REASON_DRIVER_DISCONNECT = 3,
    MINE_TELEOP_CHASSIS_STOP_REASON_SESSION_LOST = 4,
    MINE_TELEOP_CHASSIS_STOP_REASON_CONTROL_APPLY_TIMEOUT = 5,
    MINE_TELEOP_CHASSIS_STOP_REASON_OUTER_CONTROL_TIMEOUT = 6,
    MINE_TELEOP_CHASSIS_STOP_REASON_FEEDBACK_TIMEOUT = 7,
    MINE_TELEOP_CHASSIS_STOP_REASON_CONTROL_APPLY_FAILED = 8,
    MINE_TELEOP_CHASSIS_STOP_REASON_CHASSIS_CONTROL_FAULT = 9,
    MINE_TELEOP_CHASSIS_STOP_REASON_CAN_RECEIVE_FAILED = 10,
    MINE_TELEOP_CHASSIS_STOP_REASON_CAN_SEND_FAILED = 11,
    MINE_TELEOP_CHASSIS_STOP_REASON_IO_THREAD_EXCEPTION = 12,
    MINE_TELEOP_CHASSIS_STOP_REASON_HARD_OVERSPEED = 13,
    MINE_TELEOP_CHASSIS_STOP_REASON_OPPOSITE_DIRECTION_MOTION = 14,
    MINE_TELEOP_CHASSIS_STOP_REASON_ARMING_MOTION = 15,
    MINE_TELEOP_CHASSIS_STOP_REASON_CONTROL_COMMAND_INVALID = 16,
    MINE_TELEOP_CHASSIS_STOP_REASON_PHYSICAL_EMERGENCY_SWITCH = 17,
    MINE_TELEOP_CHASSIS_STOP_REASON_HANDSHAKE_REVOKED = 18,
    MINE_TELEOP_CHASSIS_STOP_REASON_CRITICAL_CAMERA_FAILED = 19,
    MINE_TELEOP_CHASSIS_STOP_REASON_MEDIA_PIPELINE_FAILED = 20,
    MINE_TELEOP_CHASSIS_STOP_REASON_VCU_STATE_FAULT = 21,
    MINE_TELEOP_CHASSIS_STOP_REASON_SESSION_PROFILE_REQUIRED = 22,
    MINE_TELEOP_CHASSIS_STOP_REASON_CAN_FEEDBACK_MISSING = 23,
    MINE_TELEOP_CHASSIS_STOP_REASON_ADAPTER_SAFETY_STATUS_UNAVAILABLE = 24,
    MINE_TELEOP_CHASSIS_STOP_REASON_LEGACY_UNSPECIFIED = 255,
};

struct MineTeleopChassisStopContextV1 {
    uint32_t struct_size;
    uint32_t stop_source;
    uint32_t stop_reason;
    uint32_t reserved;
};

struct MineTeleopChassisFeedback {
    int shake_hand_status;
    /* 20260714 forwarded EPB values: 0=hold, 1=release, 2=park. */
    int epb_status[4];
    int gear_status;
    int mcu_mode[8];
    int eps_mode[4];
    /* Degrees, matching WVCU_Str_x_Sts. */
    double eps_angle[4];
    int ehb_mode[8];
    /* SI unit used by this ABI. Raw WVCU kph is converted to m/s. */
    double vehicle_speed;
    int vehicle_speed_valid;
    /* Physical selector request from WVCU_GearCtrlReqSts. N=1, R=2, D=3. */
    int driver_gear_request;
    int driver_gear_request_valid;
};

enum MineTeleopVcuHandshakeState {
    MINE_TELEOP_VCU_STANDBY = 0,
    MINE_TELEOP_VCU_INITIAL = 1,
    MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE = 2,
    MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED = 3,
    MINE_TELEOP_VCU_WAIT_GEAR = 4,
    MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES = 5,
    MINE_TELEOP_VCU_READY = 6,
    MINE_TELEOP_VCU_DISARM_TORQUE = 7,
    MINE_TELEOP_VCU_DISARM_STOP = 8,
    MINE_TELEOP_VCU_DISARM_NEUTRAL = 9,
    MINE_TELEOP_VCU_DISARM_PARKING_BRAKE = 10,
    MINE_TELEOP_VCU_DISARM_MANUAL = 11,
    MINE_TELEOP_VCU_DISARMED = 12,
    MINE_TELEOP_VCU_FAULT = 13,
};

struct MineTeleopChassisHandshakeStatus {
    int state;
    int requested;
    int ready;
    int disarming;
    int parking_ready;
    int driver_gear_request;
    int driver_gear_request_valid;
    int handshake_status;
    int handshake_valid;
    int epb_status[4];
    int epb_valid[4];
    double speed_mps;
    int speed_valid;
    int handshake_revoked;
    int revoked_handshake_status;
    int vmc_fault_code;
    int vmc_fault_code_valid;
    int parking_brake_switch;
    int parking_brake_switch_valid;
    int brake_pedal_switch;
    int brake_pedal_switch_valid;
};

/* Complete measured CAN feedback required by ABI version 6. It remains
 * separate from MineTeleopChassisTelemetry so summary and detailed telemetry
 * have distinct responsibilities; ABI version 5 packages are not compatible.
 */
struct MineTeleopChassisCanFeedbackV1 {
    int feedback_fresh;
    long long max_feedback_age_ms;
    double speed_mps;
    int speed_valid;
    int gear;
    int gear_valid;
    int emergency_switch;
    int driver_gear_request;
    int driver_gear_request_valid;
    int handshake_status;
    int handshake_valid;
    int epb_status[4];
    int epb_valid[4];
    int motor_mode[8];
    int motor_mode_valid[8];
    double motor_torque_nm[8];
    int motor_torque_valid[8];
    double motor_speed_rpm[8];
    int motor_speed_valid[8];
    int steering_mode[4];
    int steering_valid[4];
    double steering_angle_deg[4];
    int brake_mode[8];
    int brake_valid[8];
    double brake_pressure_bar[8];
    int vmc_fault_code;
    int vmc_fault_code_valid;
    int parking_brake_switch;
    int parking_brake_switch_valid;
    int brake_pedal_switch;
    int brake_pedal_switch_valid;
};

struct MineTeleopChassisOpenConfigV1 {
    uint32_t struct_size;
    const char* can_interface;
    /* Legacy torque-scale field; the V1 compatibility path disables traction. */
    double full_scale_motor_torque_nm;
};

struct MineTeleopChassisOpenConfigV2 {
    uint32_t struct_size;
    const char* can_interface;
    /* Sole per-channel hard ceiling at normalized local-PID output=1.0. */
    double full_scale_motor_torque_nm;
    /* Independent configured vehicle-speed ceiling used only by local safety. */
    double hard_speed_limit_mps;
    /* Fresh upstream apply deadline while the VCU controller is Ready. */
    int32_t control_timeout_ms;
    int32_t speed_feedback_timeout_ms;
    double speed_pid_kp;
    double speed_pid_ki;
    double speed_pid_kd;
    double speed_pid_derivative_filter_tau_ms;
    int32_t speed_pid_max_dt_ms;
    double hard_overspeed_margin_mps;
};

/* V3 keeps the V2 prefix intact and adds the immutable physical-pressure
 * ceiling used to decode negative apply-state values. */
struct MineTeleopChassisOpenConfigV3 {
    uint32_t struct_size;
    const char* can_interface;
    double full_scale_motor_torque_nm;
    double hard_speed_limit_mps;
    int32_t control_timeout_ms;
    int32_t speed_feedback_timeout_ms;
    double speed_pid_kp;
    double speed_pid_ki;
    double speed_pid_kd;
    double speed_pid_derivative_filter_tau_ms;
    int32_t speed_pid_max_dt_ms;
    double hard_overspeed_margin_mps;
    double max_ordinary_brake_pressure_bar;
};

/* V4 keeps the V3 prefix intact and adds an optional per-motor rising-torque
 * rate. Zero disables additional slew shaping; torque reductions stay
 * immediate for every value. */
struct MineTeleopChassisOpenConfigV4 {
    uint32_t struct_size;
    const char* can_interface;
    double full_scale_motor_torque_nm;
    double hard_speed_limit_mps;
    int32_t control_timeout_ms;
    int32_t speed_feedback_timeout_ms;
    double speed_pid_kp;
    double speed_pid_ki;
    double speed_pid_kd;
    double speed_pid_derivative_filter_tau_ms;
    int32_t speed_pid_max_dt_ms;
    double hard_overspeed_margin_mps;
    double max_ordinary_brake_pressure_bar;
    double motor_torque_rise_rate_nm_per_s;
};

/* Legacy profile-version-2 runtime snapshot. Its 88-byte ABI is immutable.
 * The bridge retains the motor-torque rise rate supplied at open time. */
struct MineTeleopChassisRuntimeControlConfigV1 {
    uint32_t struct_size;
    uint32_t profile_version;
    uint64_t profile_revision;
    double target_speed_limit_mps;
    double max_motor_torque_nm;
    double max_brake_pressure_bar;
    double max_steering_request;
    double speed_pid_kp;
    double speed_pid_ki;
    double speed_pid_kd;
    double speed_pid_derivative_filter_tau_ms;
    int32_t speed_pid_max_dt_ms;
    uint32_t reserved;
};

/* Profile-version-3 runtime snapshot. V2 keeps the complete V1 prefix and
 * adds the session-scoped rising-torque rate. */
struct MineTeleopChassisRuntimeControlConfigV2 {
    uint32_t struct_size;
    uint32_t profile_version;
    uint64_t profile_revision;
    double target_speed_limit_mps;
    double max_motor_torque_nm;
    double max_brake_pressure_bar;
    double max_steering_request;
    double speed_pid_kp;
    double speed_pid_ki;
    double speed_pid_kd;
    double speed_pid_derivative_filter_tau_ms;
    int32_t speed_pid_max_dt_ms;
    uint32_t reserved;
    double motor_torque_rise_rate_nm_per_s;
};

enum MineTeleopChassisRuntimeControlIssueV1 {
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_NONE = 0,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_GENERIC_REJECTED = 1,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_RUNTIME_UNAVAILABLE = 2,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_ARGUMENTS_INVALID = 3,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_PARKING_REQUIRED = 4,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_STALE_REVISION = 5,
    MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_INTERNAL_ERROR = 6,
};

struct MineTeleopChassisRuntimeControlResultV1 {
    uint32_t struct_size;
    int32_t result_code;
    uint32_t issue_id;
    uint32_t reserved;
    uint64_t applied_revision;
};

/* Stable, string-free apply rejection identifiers. Unknown future values must
 * be treated as MINE_TELEOP_CHASSIS_APPLY_ISSUE_GENERIC_REJECTED by callers. */
enum MineTeleopChassisApplyIssueV1 {
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_NONE = 0,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_GENERIC_REJECTED = 1,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_RUNTIME_UNAVAILABLE = 2,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_PHYSICAL_EMERGENCY_LATCHED = 3,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_HARD_OVERSPEED_LATCHED = 4,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE = 5,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_ARGUMENTS_INVALID = 6,
    MINE_TELEOP_CHASSIS_APPLY_ISSUE_INTERNAL_ERROR = 7,
};

/* Versioned POD result returned atomically with apply_state_v2. The bridge
 * overwrites every field on each call, so a successful call cannot retain a
 * stale rejection identifier from an earlier attempt. */
struct MineTeleopChassisApplyResultV1 {
    uint32_t struct_size;
    int32_t result_code;
    uint32_t issue_id;
    uint32_t reserved;
};

#ifdef __cplusplus
static_assert(
    MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM <=
    -MINE_TELEOP_CHASSIS_MIN_DBC_MOTOR_TORQUE_NM * 0.8);
static_assert(
    MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM <=
    MINE_TELEOP_CHASSIS_MAX_DBC_MOTOR_TORQUE_NM * 0.8);
#define MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(field) \
    static_assert(offsetof(MineTeleopChassisOpenConfigV3, field) == \
        offsetof(MineTeleopChassisOpenConfigV2, field))
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(struct_size);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(can_interface);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(full_scale_motor_torque_nm);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(hard_speed_limit_mps);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(control_timeout_ms);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_feedback_timeout_ms);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_pid_kp);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_pid_ki);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_pid_kd);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_pid_derivative_filter_tau_ms);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(speed_pid_max_dt_ms);
MINE_TELEOP_ASSERT_V3_PREFIX_FIELD(hard_overspeed_margin_mps);
#undef MINE_TELEOP_ASSERT_V3_PREFIX_FIELD
static_assert(
    offsetof(MineTeleopChassisOpenConfigV3, max_ordinary_brake_pressure_bar) ==
    sizeof(MineTeleopChassisOpenConfigV2));
static_assert(
    sizeof(MineTeleopChassisOpenConfigV3) ==
    sizeof(MineTeleopChassisOpenConfigV2) + sizeof(double));
#define MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(field) \
    static_assert(offsetof(MineTeleopChassisOpenConfigV4, field) == \
        offsetof(MineTeleopChassisOpenConfigV3, field))
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(struct_size);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(can_interface);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(full_scale_motor_torque_nm);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(hard_speed_limit_mps);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(control_timeout_ms);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_feedback_timeout_ms);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_pid_kp);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_pid_ki);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_pid_kd);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_pid_derivative_filter_tau_ms);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(speed_pid_max_dt_ms);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(hard_overspeed_margin_mps);
MINE_TELEOP_ASSERT_V4_PREFIX_FIELD(max_ordinary_brake_pressure_bar);
#undef MINE_TELEOP_ASSERT_V4_PREFIX_FIELD
static_assert(
    offsetof(MineTeleopChassisOpenConfigV4, motor_torque_rise_rate_nm_per_s) ==
    sizeof(MineTeleopChassisOpenConfigV3));
static_assert(
    sizeof(MineTeleopChassisOpenConfigV4) ==
    sizeof(MineTeleopChassisOpenConfigV3) + sizeof(double));
static_assert(sizeof(MineTeleopChassisApplyResultV1) == 16U);
static_assert(sizeof(MineTeleopChassisStopContextV1) == 16U);
static_assert(sizeof(MineTeleopChassisTelemetry) == 64U);
static_assert(sizeof(MineTeleopChassisRuntimeControlConfigV1) == 88U);
#define MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(field) \
    static_assert(offsetof(MineTeleopChassisRuntimeControlConfigV2, field) == \
        offsetof(MineTeleopChassisRuntimeControlConfigV1, field))
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(struct_size);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(profile_version);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(profile_revision);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(target_speed_limit_mps);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(max_motor_torque_nm);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(max_brake_pressure_bar);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(max_steering_request);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(speed_pid_kp);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(speed_pid_ki);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(speed_pid_kd);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(
    speed_pid_derivative_filter_tau_ms);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(speed_pid_max_dt_ms);
MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD(reserved);
#undef MINE_TELEOP_ASSERT_RUNTIME_CONTROL_V2_PREFIX_FIELD
static_assert(
    offsetof(
        MineTeleopChassisRuntimeControlConfigV2,
        motor_torque_rise_rate_nm_per_s) ==
    sizeof(MineTeleopChassisRuntimeControlConfigV1));
static_assert(sizeof(MineTeleopChassisRuntimeControlConfigV2) == 96U);
static_assert(sizeof(MineTeleopChassisRuntimeControlResultV1) == 24U);
#endif

/* open stage/result order: -1=arguments/config/already open,
 * -2=ChassisControl/init, -4=protocol log path, -3=SocketCAN;
 * -5=unexpected exception at any stage. */
uint32_t mine_teleop_chassis_abi_version(void);
uint32_t mine_teleop_chassis_open_config_v2_size(void);
uint32_t mine_teleop_chassis_open_config_v3_size(void);
uint32_t mine_teleop_chassis_open_config_v4_size(void);
uint32_t mine_teleop_chassis_runtime_control_config_v1_size(void);
uint32_t mine_teleop_chassis_runtime_control_config_v2_size(void);
uint32_t mine_teleop_chassis_stop_context_v1_size(void);
int mine_teleop_chassis_open(const char* can_interface);
/* Versioned open used by current runtimes. Invalid size/torque is rejected
 * before ChassisControl or SocketCAN is touched. */
int mine_teleop_chassis_open_v1(
    const struct MineTeleopChassisOpenConfigV1* config);
int mine_teleop_chassis_open_v2(
    const struct MineTeleopChassisOpenConfigV2* config);
int mine_teleop_chassis_open_v3(
    const struct MineTeleopChassisOpenConfigV3* config);
int mine_teleop_chassis_open_v4(
    const struct MineTeleopChassisOpenConfigV4* config);
int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count);
/* Additive structured-result capability. The legacy apply_state entry point remains
 * available and preserves the same integer result codes. A non-null result is
 * required; it is filled in the same call while the bridge API lock is held. */
int mine_teleop_chassis_apply_state_v2(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count,
    struct MineTeleopChassisApplyResultV1* result);
/* Atomically withdraws the old traction intent, resets PID state, and installs
 * one complete session snapshot. The revision is the wire profile request seq. */
int mine_teleop_chassis_configure_runtime_control_v1(
    const struct MineTeleopChassisRuntimeControlConfigV1* config,
    struct MineTeleopChassisRuntimeControlResultV1* result);
int mine_teleop_chassis_configure_runtime_control_v2(
    const struct MineTeleopChassisRuntimeControlConfigV2* config,
    struct MineTeleopChassisRuntimeControlResultV1* result);
/* May be called from any bridge state. It never clears an existing safety
 * latch and restores the PID gains supplied by the current open config. */
int mine_teleop_chassis_clear_runtime_control_v1(
    struct MineTeleopChassisRuntimeControlResultV1* result);
/* Sets the trusted origin for the next exported stop/disconnect/close action.
 * The context is consumed once and does not itself actuate the vehicle. */
int mine_teleop_chassis_set_stop_context_v1(
    const struct MineTeleopChassisStopContextV1* context);
int mine_teleop_chassis_emergency_stop(void);
/* Start is accepted only while the selector is N, EPB is parked, speed is
 * zero, and the VCU reports manual state. */
int mine_teleop_chassis_request_parallel_handshake(void);
/* Performs the full torque/stop/N/EPB/manual reverse sequence. */
int mine_teleop_chassis_disconnect_parallel_handshake(void);
int mine_teleop_chassis_read_handshake_status(
    struct MineTeleopChassisHandshakeStatus* status);
/* Optional bench injection hook. Production feedback is drained directly from SocketCAN. */
int mine_teleop_chassis_update_feedback(const struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_poll_feedback(struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_read_telemetry(struct MineTeleopChassisTelemetry* telemetry);
int mine_teleop_chassis_read_can_feedback_v1(
    struct MineTeleopChassisCanFeedbackV1* feedback);
int mine_teleop_chassis_close(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MINE_TELEOP_CHASSIS_BRIDGE_H
