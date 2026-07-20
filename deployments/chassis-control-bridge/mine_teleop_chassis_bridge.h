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
    int epb_status[4];
    int gear_status;
    int mcu_mode[8];
    int eps_mode[4];
    double eps_angle[4];
    int ehb_mode[8];
    double vehicle_speed;
    int vehicle_speed_valid;
};

int mine_teleop_chassis_open(const char* can_interface);
int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count);
int mine_teleop_chassis_emergency_stop();
int mine_teleop_chassis_update_feedback(const struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_poll_feedback(struct MineTeleopChassisFeedback* feedback);
int mine_teleop_chassis_read_telemetry(struct MineTeleopChassisTelemetry* telemetry);
int mine_teleop_chassis_close();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MINE_TELEOP_CHASSIS_BRIDGE_H
