function(mine_teleop_chassis_diagnostics_identity target repository_root)
  set(identity "traction-diagnostics-v1")
  foreach(source IN ITEMS
      deployments/chassis-control-bridge/chassis_control_bridge.cpp
      cpp/src/vcu.cpp
      cpp/include/mine_teleop/vcu.hpp
      cpp/include/mine_teleop/detail/traction_diagnostics.hpp)
    set(path "${repository_root}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(SHA256 "${path}" digest)
    string(APPEND identity "|${source}:${digest}")
  endforeach()
  string(SHA256 digest "${identity}")
  target_compile_definitions(${target} PRIVATE MINE_TELEOP_CHASSIS_SOURCE_SHA256="${digest}")
endfunction()
