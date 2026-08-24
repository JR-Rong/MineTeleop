#include "mine_teleop_chassis_bridge.h"

#ifndef MINE_TELEOP_TEST_CHASSIS_ABI_VERSION
#error "MINE_TELEOP_TEST_CHASSIS_ABI_VERSION must be defined"
#endif

#ifndef MINE_TELEOP_TEST_CHASSIS_WRONG_V3_SIZE
#define MINE_TELEOP_TEST_CHASSIS_WRONG_V3_SIZE 0
#endif

#ifndef MINE_TELEOP_TEST_CHASSIS_HAS_APPLY_V2
#define MINE_TELEOP_TEST_CHASSIS_HAS_APPLY_V2 1
#endif

uint32_t mine_teleop_chassis_abi_version(void) {
    return MINE_TELEOP_TEST_CHASSIS_ABI_VERSION;
}

uint32_t mine_teleop_chassis_open_config_v2_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisOpenConfigV2);
}

uint32_t mine_teleop_chassis_open_config_v3_size(void) {
#if MINE_TELEOP_TEST_CHASSIS_WRONG_V3_SIZE
    return (uint32_t)(sizeof(struct MineTeleopChassisOpenConfigV3) - 1U);
#else
    return (uint32_t)sizeof(struct MineTeleopChassisOpenConfigV3);
#endif
}

int mine_teleop_chassis_open(const char* can_interface) {
    (void)can_interface;
    return 0;
}

int mine_teleop_chassis_open_v1(
    const struct MineTeleopChassisOpenConfigV1* config) {
    (void)config;
    return 0;
}

int mine_teleop_chassis_open_v2(
    const struct MineTeleopChassisOpenConfigV2* config) {
    (void)config;
    return 0;
}

int mine_teleop_chassis_open_v3(
    const struct MineTeleopChassisOpenConfigV3* config) {
    (void)config;
    return 0;
}

int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count) {
    (void)target_gear;
    (void)target_vx;
    (void)target_ax;
    (void)steering_values;
    (void)steering_count;
    return 0;
}

#if MINE_TELEOP_TEST_CHASSIS_HAS_APPLY_V2
int mine_teleop_chassis_apply_state_v2(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count,
    struct MineTeleopChassisApplyResultV1* result) {
    (void)target_gear;
    (void)target_vx;
    (void)target_ax;
    (void)steering_values;
    (void)steering_count;
    if (result != NULL) {
        result->struct_size =
            (uint32_t)sizeof(struct MineTeleopChassisApplyResultV1);
        result->result_code = 0;
        result->issue_id = MINE_TELEOP_CHASSIS_APPLY_ISSUE_NONE;
        result->reserved = 0U;
    }
    return 0;
}
#endif

int mine_teleop_chassis_emergency_stop(void) {
    return 0;
}

int mine_teleop_chassis_request_parallel_handshake(void) {
    return 0;
}

int mine_teleop_chassis_disconnect_parallel_handshake(void) {
    return 0;
}

int mine_teleop_chassis_read_handshake_status(
    struct MineTeleopChassisHandshakeStatus* status) {
    (void)status;
    return 0;
}

int mine_teleop_chassis_update_feedback(
    const struct MineTeleopChassisFeedback* feedback) {
    (void)feedback;
    return 0;
}

int mine_teleop_chassis_poll_feedback(
    struct MineTeleopChassisFeedback* feedback) {
    (void)feedback;
    return 0;
}

int mine_teleop_chassis_read_telemetry(
    struct MineTeleopChassisTelemetry* telemetry) {
    (void)telemetry;
    return 0;
}

int mine_teleop_chassis_read_can_feedback_v1(
    struct MineTeleopChassisCanFeedbackV1* feedback) {
    (void)feedback;
    return 0;
}

int mine_teleop_chassis_close(void) {
    return 0;
}
