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
  nvidia/
    lib/libcuda.so.1
    lib/libnvidia-encode.so.1
    lib/libnvcuvid.so.1
```

`deployments/cpp/Dockerfile.build` builds the matching camera C++ bridge whenever
an SDK is present, copies all runtime libraries, and sets bundle-relative
RPATHs. The chassis bridge is built with
`deployments/chassis-control-bridge/CMakeLists.txt`; place its output and every
redistributable dependency in `vendor/chassis/lib` before building the final
bundle.
The repository does not redistribute proprietary SDK binaries. Supply only the
redistributable files allowed by the Hikrobot license. Basler USB3 Vision
cameras use the source-built Aravis/libusb bridge and do not require pylon.
V4L2 and test sources work without either bridge.

NVENC field bundles also need the NVIDIA userspace libraries matching the
target kernel driver under `vendor/nvidia/lib`. The build copies them into the
strict package's `lib/` directory so GStreamer's runtime `dlopen` calls do not
fall through to libraries installed on the target host. The NVIDIA kernel
driver and device nodes remain an unavoidable host boundary; verify NVIDIA's
redistribution terms before publishing those binaries.
