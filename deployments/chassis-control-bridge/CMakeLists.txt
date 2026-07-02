cmake_minimum_required(VERSION 3.16)
project(mine_teleop_chassis_bridge LANGUAGES CXX)

set(CHASSIS_CONTROL_ROOT "/Volumes/SystemDisk/Workspace/ChassisControl" CACHE PATH "ChassisControl checkout")
set(MINEPILOT_ROOT "/Volumes/SystemDisk/Workspace/MinePilot" CACHE PATH "MinePilot checkout with low-level CAN, can_db, can_receiver, and can_sender headers")
set(MINEPILOT_CAN_COMMON_HEADER "${MINEPILOT_ROOT}/include/can/can_common.h" CACHE FILEPATH "MinePilot low-level can_common.h header")
set(MINEPILOT_CAN_MESSAGE_HEADER "${MINEPILOT_ROOT}/include/can/can_message.h" CACHE FILEPATH "MinePilot low-level can_message.h header")
set(MINEPILOT_CAN_DB_HEADER "${MINEPILOT_ROOT}/include/can_db.h" CACHE FILEPATH "MinePilot can_db.h header")
set(MINEPILOT_CAN_RECEIVER_HEADER "${MINEPILOT_ROOT}/include/can_receiver.h" CACHE FILEPATH "MinePilot can_receiver.h header")
set(MINEPILOT_CAN_SENDER_HEADER "${MINEPILOT_ROOT}/include/can_sender.h" CACHE FILEPATH "MinePilot can_sender.h header")
set(MINEPILOT_CAN_DB_SOURCE "${MINEPILOT_ROOT}/src/can_db.cpp" CACHE FILEPATH "MinePilot can_db.cpp source")
set(MINEPILOT_CAN_RECEIVER_SOURCE "${MINEPILOT_ROOT}/src/can_receiver.cpp" CACHE FILEPATH "MinePilot can_receiver.cpp source")
set(MINEPILOT_CAN_SENDER_SOURCE "${MINEPILOT_ROOT}/src/can_sender.cpp" CACHE FILEPATH "MinePilot can_sender.cpp source")

foreach(_minepilot_header
    "${MINEPILOT_CAN_COMMON_HEADER}"
    "${MINEPILOT_CAN_MESSAGE_HEADER}"
    "${MINEPILOT_CAN_DB_HEADER}"
    "${MINEPILOT_CAN_RECEIVER_HEADER}"
    "${MINEPILOT_CAN_SENDER_HEADER}")
  if(NOT EXISTS "${_minepilot_header}")
    message(FATAL_ERROR "MinePilot CAN integration header not found: ${_minepilot_header}")
  endif()
endforeach()

foreach(_minepilot_source
    "${MINEPILOT_CAN_DB_SOURCE}"
    "${MINEPILOT_CAN_RECEIVER_SOURCE}"
    "${MINEPILOT_CAN_SENDER_SOURCE}")
  if(NOT EXISTS "${_minepilot_source}")
    message(FATAL_ERROR "MinePilot CAN integration source not found: ${_minepilot_source}")
  endif()
endforeach()

find_library(CHASSIS_CONTROL_LIBRARY
  NAMES chassis_control libchassis_control
  PATHS
    "${CHASSIS_CONTROL_ROOT}/build/lib"
    "${CHASSIS_CONTROL_ROOT}/cmake-build-debug/lib"
    "${CHASSIS_CONTROL_ROOT}/cmake-build-release/lib"
    "${MINEPILOT_ROOT}"
    "${MINEPILOT_ROOT}/build/lib"
    "${MINEPILOT_ROOT}/cmake-build-debug/lib"
    "${MINEPILOT_ROOT}/cmake-build-release/lib"
  NO_DEFAULT_PATH
)

if(NOT CHASSIS_CONTROL_LIBRARY)
  message(FATAL_ERROR "ChassisControl library not found. Build ${CHASSIS_CONTROL_ROOT} first, or pass -DCHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so")
endif()

set(_default_chassis_control_api_root "${CHASSIS_CONTROL_ROOT}")
get_filename_component(_selected_chassis_library_real "${CHASSIS_CONTROL_LIBRARY}" REALPATH)
get_filename_component(_minepilot_root_real "${MINEPILOT_ROOT}" REALPATH)
string(FIND "${_selected_chassis_library_real}" "${_minepilot_root_real}/" _minepilot_library_index)
if(_minepilot_library_index EQUAL 0 AND EXISTS "${MINEPILOT_ROOT}/chassis_control.h")
  set(_default_chassis_control_api_root "${MINEPILOT_ROOT}")
endif()
if(NOT DEFINED CHASSIS_CONTROL_API_ROOT)
  set(CHASSIS_CONTROL_API_ROOT "${_default_chassis_control_api_root}" CACHE PATH "Header root for chassis_control.h matching the selected ChassisControl library")
else()
  set(CHASSIS_CONTROL_API_ROOT "${CHASSIS_CONTROL_API_ROOT}" CACHE PATH "Header root for chassis_control.h matching the selected ChassisControl library")
endif()

add_library(mine_teleop_chassis_bridge SHARED
  chassis_control_bridge.cpp
  mine_teleop_chassis_bridge.h
  "${MINEPILOT_CAN_DB_SOURCE}"
  "${MINEPILOT_CAN_RECEIVER_SOURCE}"
  "${MINEPILOT_CAN_SENDER_SOURCE}"
)
target_compile_features(mine_teleop_chassis_bridge PRIVATE cxx_std_17)
get_filename_component(_minepilot_can_common_include "${MINEPILOT_CAN_COMMON_HEADER}" DIRECTORY)
get_filename_component(_minepilot_can_message_include "${MINEPILOT_CAN_MESSAGE_HEADER}" DIRECTORY)
get_filename_component(_minepilot_can_db_include "${MINEPILOT_CAN_DB_HEADER}" DIRECTORY)
get_filename_component(_minepilot_can_receiver_include "${MINEPILOT_CAN_RECEIVER_HEADER}" DIRECTORY)
get_filename_component(_minepilot_can_sender_include "${MINEPILOT_CAN_SENDER_HEADER}" DIRECTORY)
target_include_directories(mine_teleop_chassis_bridge
  PRIVATE
    "${CHASSIS_CONTROL_API_ROOT}"
    "${CHASSIS_CONTROL_API_ROOT}/include"
    "${CHASSIS_CONTROL_ROOT}/include"
    "${MINEPILOT_ROOT}/include"
    "${_minepilot_can_common_include}"
    "${_minepilot_can_message_include}"
    "${_minepilot_can_db_include}"
    "${_minepilot_can_receiver_include}"
    "${_minepilot_can_sender_include}"
)

if(EXISTS "${CHASSIS_CONTROL_API_ROOT}/chassis_control.h")
  file(READ "${CHASSIS_CONTROL_API_ROOT}/chassis_control.h" _chassis_control_api_header)
  string(FIND "${_chassis_control_api_header}" "eps_angle" _chassis_control_eps_angle_index)
  if(NOT _chassis_control_eps_angle_index EQUAL -1)
    target_compile_definitions(mine_teleop_chassis_bridge PRIVATE MINE_TELEOP_CHASSIS_HAS_EPS_ANGLE=1)
  endif()
endif()

target_link_libraries(mine_teleop_chassis_bridge PRIVATE "${CHASSIS_CONTROL_LIBRARY}")
set_target_properties(mine_teleop_chassis_bridge PROPERTIES
  OUTPUT_NAME "mine_teleop_chassis_bridge"
)
