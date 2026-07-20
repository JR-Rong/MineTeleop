cmake_minimum_required(VERSION 3.22)

project(mine_teleop VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MINE_TELEOP_BUILD_TESTS "Build the native C++ test suite" ON)
option(MINE_TELEOP_BUILD_VENDOR_CAMERA_BRIDGES "Build camera bridges when bundled vendor SDKs are present" ON)
option(MINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE "Build the lightweight Aravis USB3 Vision camera bridge" ON)
set(MINE_TELEOP_MVS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mvs" CACHE PATH "Bundled Hikrobot MVS SDK root")

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(JPEG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-app-1.0
  gstreamer-sdp-1.0
  gstreamer-webrtc-1.0
)
include(GNUInstallDirs)

add_library(mine_teleop_runtime
  cpp/src/core.cpp
  cpp/src/http.cpp
  cpp/src/media.cpp
  cpp/src/server.cpp
  cpp/src/upload.cpp
  cpp/src/video.cpp
  cpp/src/webrtc_media.cpp
)
target_include_directories(mine_teleop_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mine_teleop_runtime
  PUBLIC
    CURL::libcurl
    JPEG::JPEG
    OpenSSL::Crypto
    Threads::Threads
    yaml-cpp
    nlohmann_json::nlohmann_json
    PkgConfig::GSTREAMER
    ${CMAKE_DL_LIBS}
)
target_compile_options(mine_teleop_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)

add_executable(mine-teleop cpp/apps/mine_teleop.cpp)
target_link_libraries(mine-teleop PRIVATE mine_teleop_runtime)
set_target_properties(mine-teleop PROPERTIES
  BUILD_RPATH "$ORIGIN/../lib"
  INSTALL_RPATH "$ORIGIN/../lib"
)

if(MINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE)
  pkg_check_modules(ARAVIS QUIET IMPORTED_TARGET aravis-0.8)
  if(ARAVIS_FOUND)
    add_executable(mine-teleop-aravis-camera cpp/bridges/aravis_camera_bridge.cpp)
    target_link_libraries(mine-teleop-aravis-camera PRIVATE PkgConfig::ARAVIS JPEG::JPEG)
    target_compile_options(mine-teleop-aravis-camera PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)
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

install(TARGETS mine-teleop mine_teleop_runtime
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY cpp/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY configs/ DESTINATION share/mine-teleop/configs)

if(MINE_TELEOP_BUILD_TESTS)
  enable_testing()
  add_executable(mine-teleop-cpp-tests cpp/tests/core_tests.cpp)
  target_link_libraries(mine-teleop-cpp-tests PRIVATE mine_teleop_runtime)
  target_compile_options(mine-teleop-cpp-tests PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)
  add_test(NAME mine-teleop-cpp-tests COMMAND mine-teleop-cpp-tests)
  set_tests_properties(mine-teleop-cpp-tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
