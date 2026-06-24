# ADR 0002: 硬件编码策略

## Status

Accepted for v1 planning.

## Context

遥操系统需要 4 路实时视频，同时还要车端保存原分辨率录像。CPU 编码可能无法稳定支撑 4 路 720p30 实时编码加录像，因此必须确认硬件编码能力。

工控机排查结果：

- 无 NVIDIA 设备或驱动。
- 有 Intel Alder Lake-S GT1 核显。
- `/dev/dri/renderD128` 存在。
- Docker 中 Ubuntu 22.04 + FFmpeg + intel-media-va-driver 已验证 `h264_vaapi` 成功编码。
- 宿主机 `/usr/local/bin/ffmpeg` 因 libva ABI 不匹配崩溃。
- 宿主机 apt/ROS 状态破损，不宜为了验证硬编直接修复。

## Decision

首版以 Intel VAAPI/QSV 作为硬件编码主线，CPU x264 作为兜底。不按 NVIDIA NVENC 作为当前工控机默认能力。

媒体运行环境优先容器化，固定 FFmpeg/GStreamer/libva/intel-media-driver 版本。

## Rationale

Intel VAAPI 已经通过实际编码测试，比仅凭 `ffmpeg -encoders` 更可靠。NVIDIA 没有硬件和驱动证据，因此不能纳入当前默认方案。

宿主机 apt/ROS 状态不稳定，容器化能隔离媒体栈依赖，降低调试风险。

## Consequences

正面：

- 当前硬件可用。
- 避免依赖不存在的 NVENC。
- 容器化后运行环境更可复现。

代价：

- 需要处理容器访问 `/dev/dri` 和相机设备的权限。
- GStreamer 硬编插件仍需验证。
- 如果最终不用容器，需要谨慎修复宿主机媒体栈。

## Follow-up

- 补测 4 路 720p30 并发 VAAPI 编码。
- 补测实时流加录像流并发。
- 验证 GStreamer Intel 硬编插件。
- 决定生产部署用容器还是修复宿主机媒体环境。

