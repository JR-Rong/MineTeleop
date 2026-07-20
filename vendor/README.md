# Bundled vendor SDK layout

The native runtime never discovers libraries from an industrial PC. Optional
camera SDKs must be provided at build time and are copied into the release
bundle:

```text
vendor/
  chassis/
    lib/libmine_teleop_chassis_bridge.so
    lib/libchassis_control.so
    lib/...other redistributable chassis libraries...
    chassis-control/...sources/headers used to build the bridge...
    minepilot/...CAN sources/headers used to build the bridge...
  mvs/
    include/MvCameraControl.h
    lib/libMvCameraControl.so
    lib/...other MVS redistributable libraries...
  pylon/
    bin/pylon-config
    include/pylon/...
    lib/pylon-redistributable-libraries
```

`deployments/cpp/Dockerfile.build` builds the matching camera C++ bridge whenever
an SDK is present, copies all runtime libraries, and sets bundle-relative
RPATHs. The chassis bridge is built with
`deployments/chassis-control-bridge/CMakeLists.txt`; place its output and every
redistributable dependency in `vendor/chassis/lib` before building the final
bundle.
The repository does not redistribute proprietary SDK binaries. Supply only the
redistributable files allowed by the Hikrobot/Basler license. V4L2 and test
sources work without either SDK.
