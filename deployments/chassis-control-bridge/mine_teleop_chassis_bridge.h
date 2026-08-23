#ifndef MINE_TELEOP_CHASSIS_BRIDGE_H
#define MINE_TELEOP_CHASSIS_BRIDGE_H

#include <float.h>
#include <math.h>
#include <stdint.h>

#define MINE_TELEOP_CHASSIS_DEFAULT_FULL_SCALE_MOTOR_TORQUE_NM 41.25
#define MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM 165.0
#define MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM 0.1
#define MINE_TELEOP_CHASSIS_MIN_SPEED_REQUEST_KPH 1.0
#define MINE_TELEOP_CHASSIS_MAX_SPEED_REQUEST_KPH 255.0
#define MINE_TELEOP_CHASSIS_DEFAULT_CONTROL_TIMEOUT_MS 800
#define MINE_TELEOP_CHASSIS_MIN_CONTROL_TIMEOUT_MS 20
#define MINE_TELEOP_CHASSIS_MAX_CONTROL_TIMEOUT_MS 60000

/* Convert the normalized longitudinal input used by the runtime into the
 * ChassisControl acceleration input. Negative values are the independent
 * braking path and are deliberately not scaled by the traction setting. */
static inline double mine_teleop_chassis_scaled_target_acceleration(
    double target_ax,
    double full_scale_motor_torque_nm) {
    return target_ax > 0.0
        ? target_ax * full_scale_motor_torque_nm /
            MINE_TELEOP_CHASSIS_DEFAULT_FULL_SCALE_MOTOR_TORQUE_NM
        : target_ax;
}

/* Final per-channel traction ceiling. ChassisControl may compensate wheel
 * force for steering angle or retain a higher prior value while rate-limiting;
 * neither is allowed to exceed the configured normalized torque request. */
static inline double mine_teleop_chassis_motor_torque_limit_nm(
    double target_ax,
    double full_scale_motor_torque_nm) {
    if (!(target_ax > 0.0) || !(full_scale_motor_torque_nm > 0.0)) return 0.0;
    const double normalized_traction = target_ax < 1.0 ? target_ax : 1.0;
    const double requested_limit = normalized_traction * full_scale_motor_torque_nm;
    return floor(
        requested_limit / MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM) *
        MINE_TELEOP_CHASSIS_MOTOR_TORQUE_RESOLUTION_NM;
}

static inline double mine_teleop_chassis_clamp_motor_torque_nm(
    double motor_torque_nm,
    double target_ax,
    double full_scale_motor_torque_nm) {
    const double limit = mine_teleop_chassis_motor_torque_limit_nm(
        target_ax, full_scale_motor_torque_nm);
    if (motor_torque_nm < -limit) return -limit;
    if (motor_torque_nm > limit) return limit;
    return motor_torque_nm;
}

/* The VCU speed request is an unsigned 1 km/h field. The apply ABI rejects
 * non-finite inputs before this conversion is used. */
static inline double mine_teleop_chassis_target_speed_request_kph(
    double target_vx_mps) {
    if (!(target_vx_mps > 0.0)) return 0.0;
    const double target_kph = target_vx_mps * 3.6;
    return target_kph < MINE_TELEOP_CHASSIS_MAX_SPEED_REQUEST_KPH
        ? target_kph
        : MINE_TELEOP_CHASSIS_MAX_SPEED_REQUEST_KPH;
}

/* A valid speed request is an active traction request, not merely a D/R gear
 * selection. Keeping this false at zero traction preserves steering and gear
 * authority without asking the VCU to actively regulate vehicle speed to 0. */
static inline int mine_teleop_chassis_vehicle_speed_request_valid(
    int target_gear,
    double target_vx_mps,
    double target_ax,
    double full_scale_motor_torque_nm) {
    return (target_gear == 2 || target_gear == 3) &&
        mine_teleop_chassis_target_speed_request_kph(target_vx_mps) >=
            MINE_TELEOP_CHASSIS_MIN_SPEED_REQUEST_KPH &&
        target_ax > 0.0 &&
        full_scale_motor_torque_nm > 0.0;
}

static inline int mine_teleop_chassis_finite(double value) {
    return value == value && value >= -DBL_MAX && value <= DBL_MAX;
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

static inline int mine_teleop_chassis_control_watchdog_expired(
    int ready,
    int has_reference,
    int already_latched,
    uint64_t elapsed_ms,
    int control_timeout_ms) {
    return ready && has_reference && !already_latched &&
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
};

/*
 * Optional versioned extension for complete measured CAN feedback. Keeping it
 * separate from MineTeleopChassisTelemetry preserves compatibility with older
 * bridge packages that expose only the summary ABI.
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
};

struct MineTeleopChassisOpenConfigV1 {
    uint32_t struct_size;
    const char* can_interface;
    /* Per motor channel at steady straight-line throttle=1.0, brake=0. */
    double full_scale_motor_torque_nm;
    /* Fresh upstream apply deadline while the VCU controller is Ready. */
    int32_t control_timeout_ms;
};

/* open: -2=ChassisControl/init, -3=SocketCAN, -4=protocol log path. */
int mine_teleop_chassis_open(const char* can_interface);
/* Versioned open used by current runtimes. Invalid size/torque is rejected
 * before ChassisControl or SocketCAN is touched. */
int mine_teleop_chassis_open_v1(
    const struct MineTeleopChassisOpenConfigV1* config);
int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count);
int mine_teleop_chassis_emergency_stop();
/* Start is accepted only while the selector is N, EPB is parked, speed is
 * zero, and the VCU reports manual state. */
int mine_teleop_chassis_request_parallel_handshake();
/* Performs the full torque/stop/N/EPB/manual reverse sequence. */
int mine_teleop_chassis_disconnect_parallel_handshake();
int mine_teleop_chassis_read_handshake_status(
    struct MineTeleopChassisHandshakeStatus* status);
/* Optional bench injection hook. Production feedback is drained directly from SocketCAN. */
int mine_teleop_chassis_update_feedback(const struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_poll_feedback(struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_read_telemetry(struct MineTeleopChassisTelemetry* telemetry);
int mine_teleop_chassis_read_can_feedback_v1(
    struct MineTeleopChassisCanFeedbackV1* feedback);
int mine_teleop_chassis_close();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MINE_TELEOP_CHASSIS_BRIDGE_H
