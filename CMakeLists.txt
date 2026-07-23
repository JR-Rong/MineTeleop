cmake_minimum_required(VERSION 3.22)

project(mine_teleop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
  set(MINE_TELEOP_VEHICLE_DEFAULT OFF)
else()
  set(MINE_TELEOP_VEHICLE_DEFAULT ON)
endif()

option(MINE_TELEOP_BUILD_TESTS "Build the native C++ test suites" ON)
option(MINE_TELEOP_BUILD_CONTROL_CLIENT "Build the portable control client" ON)
option(MINE_TELEOP_BUILD_SIGNALING_SERVER "Build the standalone signaling server" ON)
option(MINE_TELEOP_BUILD_VEHICLE_RUNTIME "Build Linux vehicle media and packaging targets" ${MINE_TELEOP_VEHICLE_DEFAULT})
option(MINE_TELEOP_BUILD_VENDOR_CAMERA_BRIDGES "Build camera bridges when bundled vendor SDKs are present" ON)
option(MINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE "Build the lightweight Aravis USB3 Vision camera bridge" ON)
option(MINE_TELEOP_FETCH_MISSING_DEPS "Fetch pinned yaml-cpp and nlohmann-json sources when absent" OFF)
set(MINE_TELEOP_MVS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mvs" CACHE PATH "Bundled Hikrobot MVS SDK root")

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
if(APPLE)
  find_library(MINE_TELEOP_SECURITY_FRAMEWORK Security REQUIRED)
else()
  find_package(OpenSSL REQUIRED)
endif()
find_package(yaml-cpp QUIET)
find_package(nlohmann_json QUIET)
if((NOT TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp) OR NOT TARGET nlohmann_json::nlohmann_json)
  if(NOT MINE_TELEOP_FETCH_MISSING_DEPS)
    message(FATAL_ERROR
      "yaml-cpp and nlohmann-json are required. Install them or configure with "
      "-DMINE_TELEOP_FETCH_MISSING_DEPS=ON to fetch the pinned sources.")
  endif()
  include(FetchContent)
  # yaml-cpp 0.8.0 still declares an older policy baseline. CMake 4 requires
  # callers to opt in explicitly when embedding that pinned release.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  if(NOT TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    FetchContent_Declare(
      yaml_cpp
      URL https://codeload.github.com/jbeder/yaml-cpp/tar.gz/f7320141120f720aecc4c32be25586e7da9eb978
      URL_HASH SHA256=2fd3bf695ccc056835a70dd6d3046312cc1004e8338b8b5c91646918a27b7ef6
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(yaml_cpp)
  endif()
  if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
      nlohmann_json
      URL https://codeload.github.com/nlohmann/json/tar.gz/65ee68451d8eb2b5f3a30b410476ab83deb3289b
      URL_HASH SHA256=13ef31d691947940a08909f8e0772f1d7d68e5da1678ee812a49c4bb0c996b2f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endif()
if(TARGET yaml-cpp::yaml-cpp)
  set(MINE_TELEOP_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(MINE_TELEOP_YAML_TARGET yaml-cpp)
endif()
include(GNUInstallDirs)

function(mine_teleop_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)
  endif()
endfunction()

# This library is the shared protocol/session/HTTP core used by all control
# packages. It intentionally has no camera, JPEG, or GStreamer dependency.
add_library(mine_teleop_runtime
  cpp/src/core.cpp
  cpp/src/http.cpp
  cpp/src/platform.cpp
  cpp/src/server.cpp
  cpp/src/websocket.cpp
)
target_include_directories(mine_teleop_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mine_teleop_runtime
  PUBLIC
    CURL::libcurl
    Threads::Threads
    ${MINE_TELEOP_YAML_TARGET}
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS}
)
if(APPLE)
  target_link_libraries(mine_teleop_runtime PUBLIC "${MINE_TELEOP_SECURITY_FRAMEWORK}")
else()
  target_link_libraries(mine_teleop_runtime PUBLIC OpenSSL::Crypto)
endif()
if(WIN32)
  target_link_libraries(mine_teleop_runtime PUBLIC ws2_32 shell32)
endif()
mine_teleop_enable_warnings(mine_teleop_runtime)

if(MINE_TELEOP_BUILD_SIGNALING_SERVER)
  add_executable(mine-teleop-signaling-server cpp/apps/mine_teleop_signaling.cpp)
  target_link_libraries(mine-teleop-signaling-server PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-signaling-server)
  if(NOT APPLE)
    install(TARGETS mine-teleop-signaling-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

if(MINE_TELEOP_BUILD_CONTROL_CLIENT)
  add_executable(mine-teleop-control cpp/apps/mine_teleop_control.cpp)
  target_link_libraries(mine-teleop-control PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-control)
  set_target_properties(mine-teleop-control PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH "$ORIGIN/../lib"
  )
  install(TARGETS mine-teleop-control RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MINE_TELEOP_BUILD_VEHICLE_RUNTIME)
  if(WIN32)
    message(FATAL_ERROR "The vehicle media runtime is Linux-only; configure with -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF")
  endif()
  find_package(JPEG REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-sdp-1.0
    gstreamer-webrtc-1.0
  )

  add_library(mine_teleop_vehicle_runtime
    cpp/src/media.cpp
    cpp/src/upload.cpp
    cpp/src/video.cpp
    cpp/src/webrtc_media.cpp
  )
  target_include_directories(mine_teleop_vehicle_runtime
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(mine_teleop_vehicle_runtime
    PUBLIC
      mine_teleop_runtime
      JPEG::JPEG
      PkgConfig::GSTREAMER
  )
  mine_teleop_enable_warnings(mine_teleop_vehicle_runtime)

  add_executable(mine-teleop cpp/apps/mine_teleop.cpp)
  target_link_libraries(mine-teleop PRIVATE mine_teleop_vehicle_runtime)
  mine_teleop_enable_warnings(mine-teleop)
  set_target_properties(mine-teleop PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH "$ORIGIN/../lib"
  )

  add_executable(mine-teleop-run cpp/apps/mine_teleop_launcher.cpp)
  mine_teleop_enable_warnings(mine-teleop-run)
  target_link_options(mine-teleop-run PRIVATE -static)

  if(MINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE)
    pkg_check_modules(ARAVIS QUIET IMPORTED_TARGET aravis-0.8)
    if(ARAVIS_FOUND)
      add_executable(mine-teleop-aravis-camera cpp/bridges/aravis_camera_bridge.cpp)
      target_link_libraries(mine-teleop-aravis-camera PRIVATE PkgConfig::ARAVIS JPEG::JPEG)
      mine_teleop_enable_warnings(mine-teleop-aravis-camera)
      set_target_properties(mine-teleop-aravis-camera PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../lib"
      )
      install(TARGETS mine-teleop-aravis-camera RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      message(STATUS "Aravis USB3 Vision bridge enabled")
    else()
      message(STATUS "Aravis 0.8 development files not found; Aravis camera bridge will not be installed")
    endif()
  endif()

  if(MINE_TELEOP_BUILD_VENDOR_CAMERA_BRIDGES)
    find_path(MINE_TELEOP_MVS_INCLUDE_DIR
      NAMES MvCameraControl.h
      HINTS "${MINE_TELEOP_MVS_SDK_ROOT}/include"
      NO_DEFAULT_PATH
    )
    find_library(MINE_TELEOP_MVS_LIBRARY
      NAMES MvCameraControl
      HINTS
        "${MINE_TELEOP_MVS_SDK_ROOT}/lib"
        "${MINE_TELEOP_MVS_SDK_ROOT}/lib/64"
        "${MINE_TELEOP_MVS_SDK_ROOT}/lib/x86_64"
      NO_DEFAULT_PATH
    )
    if(MINE_TELEOP_MVS_INCLUDE_DIR AND MINE_TELEOP_MVS_LIBRARY)
      add_executable(mine-teleop-mvs-camera cpp/bridges/mvs_camera_bridge.cpp)
      target_include_directories(mine-teleop-mvs-camera PRIVATE "${MINE_TELEOP_MVS_INCLUDE_DIR}")
      target_link_libraries(mine-teleop-mvs-camera PRIVATE "${MINE_TELEOP_MVS_LIBRARY}")
      set_target_properties(mine-teleop-mvs-camera PROPERTIES INSTALL_RPATH "$ORIGIN/../lib/vendor/mvs")
      install(TARGETS mine-teleop-mvs-camera RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      install(DIRECTORY "${MINE_TELEOP_MVS_SDK_ROOT}/lib/" DESTINATION ${CMAKE_INSTALL_LIBDIR}/vendor/mvs
        FILES_MATCHING PATTERN "*.so" PATTERN "*.so.*")
      message(STATUS "Hikrobot MVS bridge enabled from ${MINE_TELEOP_MVS_SDK_ROOT}")
    else()
      message(STATUS "Hikrobot MVS SDK not bundled; MVS bridge will not be installed")
    endif()
  endif()

  install(TARGETS mine-teleop mine-teleop-run mine_teleop_vehicle_runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

install(TARGETS mine_teleop_runtime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY cpp/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY configs/ DESTINATION share/mine-teleop/configs)
install(DIRECTORY protocol/ DESTINATION share/mine-teleop/protocol)

if(MINE_TELEOP_BUILD_TESTS)
  enable_testing()
  if(MINE_TELEOP_BUILD_SIGNALING_SERVER)
    add_test(NAME mine-teleop-signaling-help COMMAND mine-teleop-signaling-server --help)
    set_tests_properties(
      mine-teleop-signaling-help
      PROPERTIES PASS_REGULAR_EXPRESSION "Usage:.*mine-teleop-signaling-server"
    )
    add_test(NAME mine-teleop-signaling-version COMMAND mine-teleop-signaling-server --version)
    set_tests_properties(
      mine-teleop-signaling-version
      PROPERTIES PASS_REGULAR_EXPRESSION "mine-teleop-signaling-server 0\\.2\\.0"
    )
    add_test(
      NAME mine-teleop-signaling-validates-multi-identity-config
      COMMAND ${CMAKE_COMMAND} -E env
        --unset=MINE_TELEOP_DRIVER_PASSWORD
        --unset=MINE_TELEOP_DEVICE_TOKEN
        MINE_TELEOP_DRIVER_001_PASSWORD=test-driver-1
        MINE_TELEOP_DRIVER_002_PASSWORD=test-driver-2
        MINE_TELEOP_VEHICLE_001_TOKEN=test-vehicle-1
        MINE_TELEOP_VEHICLE_002_TOKEN=test-vehicle-2
        $<TARGET_FILE:mine-teleop-signaling-server>
        --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/signaling-server.2x2.dev.yaml
        --validate-config
    )
    set_tests_properties(
      mine-teleop-signaling-validates-multi-identity-config
      PROPERTIES
        PASS_REGULAR_EXPRESSION "\"driver_count\":2.*\"permission_count\":2.*\"vehicle_count\":2"
    )
    add_test(
      NAME mine-teleop-signaling-validates-browser-switch-config
      COMMAND ${CMAKE_COMMAND} -E env
        --unset=MINE_TELEOP_DRIVER_PASSWORD
        --unset=MINE_TELEOP_DEVICE_TOKEN
        MINE_TELEOP_DRIVER_001_PASSWORD=test-driver-1
        MINE_TELEOP_DRIVER_002_PASSWORD=test-driver-2
        MINE_TELEOP_VEHICLE_001_TOKEN=test-vehicle-1
        MINE_TELEOP_VEHICLE_002_TOKEN=test-vehicle-2
        $<TARGET_FILE:mine-teleop-signaling-server>
        --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/signaling-server.browser-switch.dev.yaml
        --validate-config
    )
    set_tests_properties(
      mine-teleop-signaling-validates-browser-switch-config
      PROPERTIES
        PASS_REGULAR_EXPRESSION "\"driver_count\":2.*\"permission_count\":3.*\"vehicle_count\":2"
    )
    add_test(
      NAME mine-teleop-signaling-rejects-mixed-identity-modes
      COMMAND mine-teleop-signaling-server
        --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/signaling-server.2x2.dev.yaml
        --driver-id driver-console-001
        --validate-config
    )
    set_tests_properties(mine-teleop-signaling-rejects-mixed-identity-modes PROPERTIES WILL_FAIL TRUE)
    add_test(
      NAME mine-teleop-signaling-rejects-unknown-option
      COMMAND mine-teleop-signaling-server --definitely-not-an-option
    )
    set_tests_properties(mine-teleop-signaling-rejects-unknown-option PROPERTIES WILL_FAIL TRUE)
    add_test(
      NAME mine-teleop-signaling-rejects-public-bind
      COMMAND mine-teleop-signaling-server --host 0.0.0.0 --port 0
    )
    set_tests_properties(mine-teleop-signaling-rejects-public-bind PROPERTIES WILL_FAIL TRUE)
  endif()
  add_executable(mine-teleop-control-tests cpp/tests/control_tests.cpp)
  target_link_libraries(mine-teleop-control-tests PRIVATE mine_teleop_runtime)
  mine_teleop_enable_warnings(mine-teleop-control-tests)
  add_test(NAME mine-teleop-control-tests COMMAND mine-teleop-control-tests)
  set_tests_properties(mine-teleop-control-tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  if(UNIX)
    add_test(
      NAME mine-teleop-coturn-usage-report-self-test
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/coturn_usage_report.sh --self-test
    )
    add_test(
      NAME mine-teleop-turnserver-config-render-self-test
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/render_turnserver_config.sh --self-test
    )
  endif()

  if(MINE_TELEOP_BUILD_VEHICLE_RUNTIME)
    add_executable(mine-teleop-cpp-tests cpp/tests/core_tests.cpp)
    target_link_libraries(mine-teleop-cpp-tests PRIVATE mine_teleop_vehicle_runtime)
    mine_teleop_enable_warnings(mine-teleop-cpp-tests)
    add_test(NAME mine-teleop-cpp-tests COMMAND mine-teleop-cpp-tests)
    set_tests_properties(mine-teleop-cpp-tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
