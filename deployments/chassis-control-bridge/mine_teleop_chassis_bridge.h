#ifndef MINE_TELEOP_CHASSIS_BRIDGE_H
#define MINE_TELEOP_CHASSIS_BRIDGE_H

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

/* open: -2=ChassisControl/init, -3=SocketCAN, -4=protocol log path. */
int mine_teleop_chassis_open(const char* can_interface);
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
int mine_teleop_chassis_close();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MINE_TELEOP_CHASSIS_BRIDGE_H
