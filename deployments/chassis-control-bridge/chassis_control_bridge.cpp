#include "chassis_control.h"
#include "can/can_common.h"
#include "can_db.h"
#include "can_receiver.h"
#include "mine_teleop_chassis_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr int kDefaultWheelCount = 8;
constexpr double kDefaultWheelRadiusM = 0.55;
constexpr double kDefaultTrackM = 2.2;
constexpr double kDefaultWheelBaseM = 4.4;

std::mutex g_mutex;
std::string g_can_interface;
can_handle_t g_can_handle = nullptr;
ArmingState g_arming_state = ArmingState::ARMED_INITIAL;
ArmingFeedback g_arming_feedback;
MineTeleopChassisFeedback g_last_feedback{};
MineTeleopChassisTelemetry g_last_telemetry{0.0, 1, 0.0, 0.0, 0.0, 0};

float clamp_float(double value, double minimum, double maximum)
{
    return static_cast<float>(std::max(minimum, std::min(maximum, value)));
}

WheelParam make_default_wheel_param(int index)
{
    WheelParam wheel{};
    wheel.feture_name = "default";
    wheel.mu = 0.7f;
    wheel.slip_threshold = 0.2f;
    wheel.wheel_width = 0.4f;
    wheel.wheel_radius = static_cast<float>(kDefaultWheelRadiusM);
    wheel.wheel_pressure = 0.0f;
    wheel.max_electric_torque = 2500.0f;
    wheel.max_genera_torque = 1500.0f;

    const int axle = index / 2;
    const bool left = (index % 2) == 0;
    const float x = static_cast<float>((1.5 - axle) * (kDefaultWheelBaseM / 3.0));
    const float y = static_cast<float>(left ? kDefaultTrackM / 2.0 : -kDefaultTrackM / 2.0);
    wheel.wheel_position = {x, y, 0.0f};
    return wheel;
}

VehicleParam make_default_vehicle_param()
{
    VehicleParam vehicle{};
    vehicle.vechicle_static_param.wheel_count = kDefaultWheelCount;
    vehicle.vechicle_dynamic_param.mass = 18000.0f;
    vehicle.vechicle_dynamic_param.cog_position = {0.0f, 0.0f, 1.2f};
    vehicle.vechicle_dynamic_param.wheel_params.clear();
    for (int index = 0; index < kDefaultWheelCount; ++index) {
        vehicle.vechicle_dynamic_param.wheel_params.push_back(make_default_wheel_param(index));
    }
    return vehicle;
}

VehicleState make_vehicle_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count)
{
    VehicleState state{};
    state.cur_velocity = static_cast<float>(g_last_telemetry.speed_mps);
    state.target_velocity = {clamp_float(target_vx, 0.0, 20.0), 0.0f};
    state.target_acceleration = {clamp_float(target_ax, -8.0, 4.0), 0.0f};
    state.target_gear = target_gear;
    state.target_position = {0.0f, 0.0f, 0.0f};
    state.vehicle_posture = {0.0f, 0.0f, 0.0f};
    state.vehicle_position = {0.0f, 0.0f, 0.0f};
    state.target_steering_angle.assign(kDefaultWheelCount, 0.0f);
    const int count = std::max(0, std::min(steering_count, kDefaultWheelCount));
    for (int index = 0; index < count; ++index) {
        state.target_steering_angle[index] = clamp_float(steering_values[index], -1.2, 1.2);
    }
    state.tier_state.assign(kDefaultWheelCount, WheelState{});
    return state;
}

bool ensure_can_open(const char* can_interface)
{
    if (g_can_handle != nullptr) {
        return true;
    }
    can_config_t config{};
    config.bitrate = 500000;
    config.sample_point = 80;
    config.sjw = 1;
    config.prop_seg = 1;
    config.phase_seg1 = 6;
    config.phase_seg2 = 3;
    config.listen_only = false;
    config.loopback = false;

    g_can_handle = can_open(can_interface, &config);
    if (g_can_handle == nullptr) {
        return false;
    }
    get_can_handle() = g_can_handle;
    return true;
}

void apply_feedback_unlocked(const MineTeleopChassisFeedback& feedback)
{
    g_last_feedback = feedback;
    g_arming_feedback.shake_hand_status = feedback.shake_hand_status;
    g_arming_feedback.epb1_status = feedback.epb_status[0];
    g_arming_feedback.epb2_status = feedback.epb_status[1];
    g_arming_feedback.epb3_status = feedback.epb_status[2];
    g_arming_feedback.epb4_status = feedback.epb_status[3];
    g_arming_feedback.gear_status = feedback.gear_status;
    for (int index = 0; index < 8; ++index) {
        g_arming_feedback.mcu_mode[index] = feedback.mcu_mode[index];
        g_arming_feedback.ehb_mode[index] = feedback.ehb_mode[index];
    }
    for (int index = 0; index < 4; ++index) {
        g_arming_feedback.eps_mode[index] = feedback.eps_mode[index];
#ifdef MINE_TELEOP_CHASSIS_HAS_EPS_ANGLE
        g_arming_feedback.eps_angle[index] = std::isnan(feedback.eps_angle[index])
            ? std::numeric_limits<float>::quiet_NaN()
            : static_cast<float>(feedback.eps_angle[index]);
#endif
    }
    g_arming_feedback.vehicle_speed = static_cast<float>(feedback.vehicle_speed);
    g_arming_feedback.vehicle_speed_valid = feedback.vehicle_speed_valid != 0;
    if (g_arming_feedback.vehicle_speed_valid) {
        g_last_telemetry.speed_mps = feedback.vehicle_speed;
    }
    g_last_telemetry.gear = feedback.gear_status;
    // Steering feedback is available on the CAN set (eps_angle); prefer it over
    // the commanded angle so telemetry reflects the actual wheel state.
    if (!std::isnan(feedback.eps_angle[0])) {
        g_last_telemetry.steering_feedback = feedback.eps_angle[0];
    }
}

bool decode_feedback_frame(const can_frame_t& frame, MineTeleopChassisFeedback* feedback)
{
    if (feedback == nullptr) {
        return false;
    }
    std::uint8_t can_bytes[8]{};
    const int raw_dlc = static_cast<int>(frame.can_dlc);
    const std::size_t dlc = static_cast<std::size_t>(std::max(0, std::min(raw_dlc, 8)));
    std::memcpy(can_bytes, frame.data, dlc);

    switch (frame.can_id) {
        case CAN_DB_WVCU_VEH_SHAKE_HAND_STS_FRAME_ID: {
            can_db_wvcu_veh_shake_hand_sts_t raw{};
            if (can_db_wvcu_veh_shake_hand_sts_unpack(&raw, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->shake_hand_status = static_cast<int>(
                can_db_wvcu_veh_shake_hand_sts_wvcu_shake_hand_sts_decode(raw.wvcu_shake_hand_sts));
            return true;
        }
        case CAN_DB_WVCU_VCU_STS_FRAME_ID: {
            can_db_wvcu_vcu_sts_d decoded{};
            if (can_db_wvcu_vcu_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->gear_status = static_cast<int>(decoded.wvcu_gear_sts_now);
            return true;
        }
        case CAN_DB_WVCU_PRK_1_STS_FRAME_ID: {
            can_db_wvcu_prk_1_sts_d decoded{};
            if (can_db_wvcu_prk_1_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->epb_status[0] = static_cast<int>(decoded.prk_1_sts01_mode);
            feedback->epb_status[1] = static_cast<int>(decoded.prk_1_sts02_mode);
            feedback->epb_status[2] = static_cast<int>(decoded.prk_1_sts03_mode);
            feedback->epb_status[3] = static_cast<int>(decoded.prk_1_sts04_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_1_STS02_FRAME_ID: {
            can_db_wvcu_mot_1_sts02_d decoded{};
            if (can_db_wvcu_mot_1_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[0] = static_cast<int>(decoded.mot_1_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_2_STS02_FRAME_ID: {
            can_db_wvcu_mot_2_sts02_d decoded{};
            if (can_db_wvcu_mot_2_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[1] = static_cast<int>(decoded.mot_2_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_3_STS02_FRAME_ID: {
            can_db_wvcu_mot_3_sts02_d decoded{};
            if (can_db_wvcu_mot_3_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[2] = static_cast<int>(decoded.mot_3_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_4_STS02_FRAME_ID: {
            can_db_wvcu_mot_4_sts02_d decoded{};
            if (can_db_wvcu_mot_4_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[3] = static_cast<int>(decoded.mot_4_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_5_STS02_FRAME_ID: {
            can_db_wvcu_mot_5_sts02_d decoded{};
            if (can_db_wvcu_mot_5_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[4] = static_cast<int>(decoded.mot_5_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_6_STS02_FRAME_ID: {
            can_db_wvcu_mot_6_sts02_d decoded{};
            if (can_db_wvcu_mot_6_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[5] = static_cast<int>(decoded.mot_6_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_7_STS02_FRAME_ID: {
            can_db_wvcu_mot_7_sts02_d decoded{};
            if (can_db_wvcu_mot_7_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[6] = static_cast<int>(decoded.mot_7_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_MOT_8_STS02_FRAME_ID: {
            can_db_wvcu_mot_8_sts02_d decoded{};
            if (can_db_wvcu_mot_8_sts02_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->mcu_mode[7] = static_cast<int>(decoded.mot_8_sts02_mot_work_mode);
            return true;
        }
        case CAN_DB_WVCU_STR_1_STS_FRAME_ID: {
            can_db_wvcu_str_1_sts_d decoded{};
            if (can_db_wvcu_str_1_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->eps_mode[0] = static_cast<int>(decoded.str_1_mode_sts);
            feedback->eps_angle[0] = decoded.str_1_ang_sts;
            return true;
        }
        case CAN_DB_WVCU_STR_2_STS__FRAME_ID: {
            can_db_wvcu_str_2_sts__d decoded{};
            if (can_db_wvcu_str_2_sts__decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->eps_mode[1] = static_cast<int>(decoded.str_2_mode_sts);
            feedback->eps_angle[1] = decoded.str_2_ang_sts;
            return true;
        }
        case CAN_DB_WVCU_STR_3_STS_FRAME_ID: {
            can_db_wvcu_str_3_sts_d decoded{};
            if (can_db_wvcu_str_3_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->eps_mode[2] = static_cast<int>(decoded.str_3_mode_sts);
            feedback->eps_angle[2] = decoded.str_3_ang_sts;
            return true;
        }
        case CAN_DB_WVCU_STR_4_STS_FRAME_ID: {
            can_db_wvcu_str_4_sts_d decoded{};
            if (can_db_wvcu_str_4_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->eps_mode[3] = static_cast<int>(decoded.str_4_mode_sts);
            feedback->eps_angle[3] = decoded.str_4_ang_sts;
            return true;
        }
        case CAN_DB_WVCU_BRK_1_STS_FRAME_ID: {
            can_db_wvcu_brk_1_sts_d decoded{};
            if (can_db_wvcu_brk_1_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->ehb_mode[0] = static_cast<int>(decoded.brk_1_sts01_mode);
            feedback->ehb_mode[1] = static_cast<int>(decoded.brk_1_sts02_mode);
            return true;
        }
        case CAN_DB_WVCU_BRK_3_STS_FRAME_ID: {
            can_db_wvcu_brk_3_sts_d decoded{};
            if (can_db_wvcu_brk_3_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->ehb_mode[2] = static_cast<int>(decoded.brk_3_sts03_mode);
            feedback->ehb_mode[3] = static_cast<int>(decoded.brk_3_sts04_mode);
            return true;
        }
        case CAN_DB_WVCU_BRK_5_STS_FRAME_ID: {
            can_db_wvcu_brk_5_sts_d decoded{};
            if (can_db_wvcu_brk_5_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->ehb_mode[4] = static_cast<int>(decoded.brk_5_sts05_mode);
            feedback->ehb_mode[5] = static_cast<int>(decoded.brk_5_sts06_mode);
            return true;
        }
        case CAN_DB_WVCU_BRK_7_STS_FRAME_ID: {
            can_db_wvcu_brk_7_sts_d decoded{};
            if (can_db_wvcu_brk_7_sts_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->ehb_mode[6] = static_cast<int>(decoded.brk_7_sts07_mode);
            feedback->ehb_mode[7] = static_cast<int>(decoded.brk_7_sts08_mode);
            return true;
        }
        case CAN_DB_WVCU_VEH_SPD_STS_NOW_FRAME_ID: {
            can_db_wvcu_veh_spd_sts_now_d decoded{};
            if (can_db_wvcu_veh_spd_sts_now_decode(&decoded, can_bytes, dlc) != 0) {
                return false;
            }
            feedback->vehicle_speed = decoded.wvcu_veh_spd_now;
            feedback->vehicle_speed_valid = 1;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

extern "C" int mine_teleop_chassis_open(const char* can_interface)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (can_interface == nullptr || can_interface[0] == '\0') {
        return -1;
    }
    g_can_interface = can_interface;
    VehicleParam vehicle = make_default_vehicle_param();
    if (!Initialize(vehicle, g_can_interface)) {
        return -2;
    }
    if (!ensure_can_open(g_can_interface.c_str())) {
        return -3;
    }
    ResetArmingStateMachine();
    ResetDisarmSequence();
    g_arming_state = ArmingState::ARMED_INITIAL;
    g_last_feedback = MineTeleopChassisFeedback{};
    g_last_telemetry = MineTeleopChassisTelemetry{0.0, 1, 0.0, 0.0, 0.0, 0};
    return 0;
}

extern "C" int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_can_interface.empty() || g_can_handle == nullptr || steering_values == nullptr || steering_count < 0) {
        return -1;
    }
    VehicleState state = make_vehicle_state(target_gear, target_vx, target_ax, steering_values, steering_count);
    if (!UpdateVehicleState(state)) {
        return -2;
    }
    g_arming_state = RunArmingStateMachine(g_arming_state, g_arming_feedback, true);
    if (!SendCanMessage(g_can_interface, 0)) {
        return -3;
    }
    // NOTE: speed_mps, gear and steering_feedback are populated ONLY from decoded
    // CAN feedback (see apply_feedback_unlocked) and must never be set from the
    // command here, otherwise telemetry would show the vehicle "obeying" even when
    // the chassis rejected the command. The CAN feedback set carries no dedicated
    // throttle/brake actuator magnitude, so these two fields expose the commanded
    // longitudinal effort as a best-effort indication until such feedback exists.
    g_last_telemetry.throttle_feedback = target_ax > 0.0 ? std::min(target_ax, 1.0) : 0.0;
    g_last_telemetry.brake_feedback = target_ax < 0.0 ? std::min(std::abs(target_ax), 1.0) : 0.0;
    // Normal control has resumed; clear the e-stop indication. In the correct flow
    // apply_state is never invoked while an e-stop is latched (the safety layer
    // routes to emergency_stop instead).
    g_last_telemetry.estop = 0;
    return 0;
}

extern "C" int mine_teleop_chassis_emergency_stop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_can_interface.empty() || g_can_handle == nullptr) {
        return -1;
    }
    if (!EmergencyStopWheels()) {
        return -2;
    }
    g_last_telemetry.speed_mps = 0.0;
    g_last_telemetry.throttle_feedback = 0.0;
    g_last_telemetry.brake_feedback = 1.0;
    g_last_telemetry.estop = 1;
    return 0;
}

extern "C" int mine_teleop_chassis_update_feedback(const MineTeleopChassisFeedback* feedback)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (feedback == nullptr) {
        return -1;
    }
    apply_feedback_unlocked(*feedback);
    return 0;
}

extern "C" int mine_teleop_chassis_poll_feedback(MineTeleopChassisFeedback* feedback)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (feedback == nullptr) {
        return -1;
    }
    if (g_can_interface.empty()) {
        return -2;
    }
    if (g_can_handle == nullptr) {
        return -3;
    }

    MineTeleopChassisFeedback next_feedback = g_last_feedback;
    can_frame_t rx_frame{};
    const int receive_result = can_receive(g_can_handle, &rx_frame, 0);
    if (receive_result == CAN_ERR_TIMEOUT) {
        *feedback = g_last_feedback;
        return 1;
    }
    if (receive_result != CAN_SUCCESS) {
        return receive_result;
    }
    if (!decode_feedback_frame(rx_frame, &next_feedback)) {
        *feedback = g_last_feedback;
        return 1;
    }
    apply_feedback_unlocked(next_feedback);
    *feedback = g_last_feedback;
    return 0;
}

extern "C" int mine_teleop_chassis_read_telemetry(MineTeleopChassisTelemetry* telemetry)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (telemetry == nullptr) {
        return -1;
    }
    *telemetry = g_last_telemetry;
    return 0;
}

extern "C" int mine_teleop_chassis_close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    int result = 0;
    if (g_can_handle != nullptr) {
        result = can_close(g_can_handle);
        g_can_handle = nullptr;
        get_can_handle() = nullptr;
    }
    g_can_interface.clear();
    return result;
}
