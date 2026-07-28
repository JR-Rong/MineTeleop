cmake_minimum_required(VERSION 3.16)
project(mine_teleop_chassis_bridge LANGUAGES CXX)

get_filename_component(MINE_TELEOP_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MINE_TELEOP_CHASSIS_VENDOR_ROOT "${MINE_TELEOP_REPOSITORY_ROOT}/vendor/chassis" CACHE PATH "Bundled chassis integration root")
set(CHASSIS_CONTROL_ROOT "${MINE_TELEOP_CHASSIS_VENDOR_ROOT}/chassis-control" CACHE PATH "Bundled ChassisControl source/header root")

find_library(CHASSIS_CONTROL_LIBRARY
  NAMES chassis_control libchassis_control
  PATHS
    "${CHASSIS_CONTROL_ROOT}/build/lib"
    "${CHASSIS_CONTROL_ROOT}/cmake-build-debug/lib"
    "${CHASSIS_CONTROL_ROOT}/cmake-build-release/lib"
  NO_DEFAULT_PATH
)

if(NOT CHASSIS_CONTROL_LIBRARY)
  message(FATAL_ERROR "ChassisControl library not found. Build ${CHASSIS_CONTROL_ROOT} first, or pass -DCHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so")
endif()

if(NOT EXISTS "${CHASSIS_CONTROL_ROOT}/include/global_variables.h")
  message(FATAL_ERROR
    "ChassisControl source headers are required to read the calculated eight-wheel control output: "
    "${CHASSIS_CONTROL_ROOT}/include/global_variables.h")
endif()

find_package(Threads REQUIRED)

add_library(mine_teleop_chassis_bridge SHARED
  chassis_control_bridge.cpp
  mine_teleop_chassis_bridge.h
  "${MINE_TELEOP_REPOSITORY_ROOT}/cpp/src/vcu.cpp"
)
target_compile_features(mine_teleop_chassis_bridge PRIVATE cxx_std_17)
target_compile_options(
  mine_teleop_chassis_bridge
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
)
target_include_directories(mine_teleop_chassis_bridge
  PRIVATE
    "${MINE_TELEOP_REPOSITORY_ROOT}/cpp/include"
    "${CHASSIS_CONTROL_ROOT}"
    "${CHASSIS_CONTROL_ROOT}/include"
    "${CHASSIS_CONTROL_ROOT}/include/Vehicle_Control"
    "${CHASSIS_CONTROL_ROOT}/include/Vehicle_Data_Input"
    "${CHASSIS_CONTROL_ROOT}/include/Vehicle_Data_Output"
    "${CHASSIS_CONTROL_ROOT}/include/Log"
    "${CHASSIS_CONTROL_ROOT}/include/can"
)

target_link_libraries(
  mine_teleop_chassis_bridge
  PRIVATE
    "${CHASSIS_CONTROL_LIBRARY}"
    Threads::Threads
)
set_target_properties(mine_teleop_chassis_bridge PROPERTIES
  OUTPUT_NAME "mine_teleop_chassis_bridge"
  INSTALL_RPATH "$ORIGIN"
)

include(GNUInstallDirs)
install(TARGETS mine_teleop_chassis_bridge LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/vendor/chassis)
