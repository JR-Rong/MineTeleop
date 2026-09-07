#include "mine_teleop_chassis_bridge.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef MINE_TELEOP_ABI_FIXTURE_VERSION
#define MINE_TELEOP_ABI_FIXTURE_VERSION 6U
#endif

#ifndef MINE_TELEOP_ABI_FIXTURE_HAS_APPLY_STATE_V2
#define MINE_TELEOP_ABI_FIXTURE_HAS_APPLY_STATE_V2 1
#endif

#ifndef MINE_TELEOP_ABI_FIXTURE_HAS_LEGACY_APPLY_STATE
#define MINE_TELEOP_ABI_FIXTURE_HAS_LEGACY_APPLY_STATE 1
#endif

#ifndef MINE_TELEOP_ABI_FIXTURE_HAS_READ_TELEMETRY
#define MINE_TELEOP_ABI_FIXTURE_HAS_READ_TELEMETRY 1
#endif

#ifndef MINE_TELEOP_ABI_FIXTURE_WRONG_V4_SIZE
#define MINE_TELEOP_ABI_FIXTURE_WRONG_V4_SIZE 0
#endif

static void mark_can_initialization(void) {
    const char* const marker_path = getenv("MINE_TELEOP_ABI_PRECHECK_MARKER");
    if (marker_path == NULL || marker_path[0] == '\0') return;
    FILE* const marker = fopen(marker_path, "w");
    if (marker == NULL) return;
    fputs("mine_teleop_chassis_open invoked\n", marker);
    fclose(marker);
}

uint32_t mine_teleop_chassis_abi_version(void) {
    return MINE_TELEOP_ABI_FIXTURE_VERSION;
}

uint32_t mine_teleop_chassis_open_config_v2_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisOpenConfigV2);
}

uint32_t mine_teleop_chassis_open_config_v3_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisOpenConfigV3);
}

uint32_t mine_teleop_chassis_open_config_v4_size(void) {
#if MINE_TELEOP_ABI_FIXTURE_WRONG_V4_SIZE
    return (uint32_t)(sizeof(struct MineTeleopChassisOpenConfigV4) - 1U);
#else
    return (uint32_t)sizeof(struct MineTeleopChassisOpenConfigV4);
#endif
}

uint32_t mine_teleop_chassis_runtime_control_config_v1_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisRuntimeControlConfigV1);
}

uint32_t mine_teleop_chassis_runtime_control_config_v2_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisRuntimeControlConfigV2);
}

uint32_t mine_teleop_chassis_stop_context_v1_size(void) {
    return (uint32_t)sizeof(struct MineTeleopChassisStopContextV1);
}

int mine_teleop_chassis_open(const char* can_interface) {
    (void)can_interface;
    mark_can_initialization();
    return 0;
}

int mine_teleop_chassis_open_v1(
    const struct MineTeleopChassisOpenConfigV1* config) {
    (void)config;
    mark_can_initialization();
    return 0;
}

int mine_teleop_chassis_open_v2(
    const struct MineTeleopChassisOpenConfigV2* config) {
    (void)config;
    mark_can_initialization();
    return 0;
}

int mine_teleop_chassis_open_v3(
    const struct MineTeleopChassisOpenConfigV3* config) {
    (void)config;
    mark_can_initialization();
    return 0;
}

int mine_teleop_chassis_open_v4(
    const struct MineTeleopChassisOpenConfigV4* config) {
    (void)config;
    mark_can_initialization();
    return 0;
}

#if MINE_TELEOP_ABI_FIXTURE_HAS_LEGACY_APPLY_STATE
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
#endif

#if MINE_TELEOP_ABI_FIXTURE_HAS_APPLY_STATE_V2
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
    (void)result;
    return 0;
}
#endif

int mine_teleop_chassis_configure_runtime_control_v1(
    const struct MineTeleopChassisRuntimeControlConfigV1* config,
    struct MineTeleopChassisRuntimeControlResultV1* result) {
    (void)config;
    (void)result;
    return 0;
}

int mine_teleop_chassis_configure_runtime_control_v2(
    const struct MineTeleopChassisRuntimeControlConfigV2* config,
    struct MineTeleopChassisRuntimeControlResultV1* result) {
    (void)config;
    (void)result;
    return 0;
}

int mine_teleop_chassis_clear_runtime_control_v1(
    struct MineTeleopChassisRuntimeControlResultV1* result) {
    (void)result;
    return 0;
}

int mine_teleop_chassis_set_stop_context_v1(
    const struct MineTeleopChassisStopContextV1* context) {
    (void)context;
    return 0;
}

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

int mine_teleop_chassis_poll_feedback(struct MineTeleopChassisFeedback* feedback) {
    (void)feedback;
    return 0;
}

#if MINE_TELEOP_ABI_FIXTURE_HAS_READ_TELEMETRY
int mine_teleop_chassis_read_telemetry(struct MineTeleopChassisTelemetry* telemetry) {
    (void)telemetry;
    return 0;
}
#endif

int mine_teleop_chassis_read_can_feedback_v1(
    struct MineTeleopChassisCanFeedbackV1* feedback) {
    (void)feedback;
    return 0;
}

int mine_teleop_chassis_close(void) {
    return 0;
}
