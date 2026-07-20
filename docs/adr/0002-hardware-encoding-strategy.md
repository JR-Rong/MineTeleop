# ADR 0002: 硬件编码策略

## Status

Accepted and implemented.

## Context

遥操系统需要 4 路实时视频，同时还要车端保存原分辨率录像。CPU 编码可能无法稳定支撑 4 路 720p30 实时编码加录像，因此必须确认硬件编码能力。

2026-07-20 台架实机排查结果：

- NVIDIA RTX 2000 Ada、驱动 595.71.05 可见。
- Intel Alder Lake 核显与 `/dev/dri/renderD128` 可见。
- NVENC/VAAPI 的 H.264、H.265 720p30 编码均已通过。
- Chrome H.265 单轨连续解码通过；本次驾驶端硬件同时解码 4 路 H.265 时只有
  首轨稳定，因此不能仅凭 codec capability 判定多路可用。

## Decision

统一 `VideoEncoder` 接口按以下顺序选择：

1. 首选 codec 的 NVIDIA NVENC。
2. 编码器故障时，切到同 codec 的 Intel VAAPI。
3. 浏览器 H.265 任一路连续 3 个统计周期低于实时帧率门限时，切到 H.264
   NVENC；若它失败，再切到 H.264 VAAPI。

默认 codec 为 H.265，fallback 为 H.264。生产配置禁止 CPU x264 静默兜底。
实时媒体、RTP/WebRTC 和录像复用均由同一进程内的 GStreamer pipeline 完成。

## Rationale

NVENC 是台架上的首选低延迟路径；Intel 核显独立于 NVIDIA GPU，可作为 GPU
故障后的硬件回退。H.265 可以在相近画质下降低码率，但不能假定所有浏览器
都支持 WebRTC H.265，所以能力协商和 H.264 回退不可删除。

## Consequences

正面：

- 编码后端与 codec 回退可被独立测试。
- 录像直接复用实时编码结果，不增加第二次编码延迟或 GPU 负载。
- 严格 x64 测试包携带 GStreamer、VAAPI 和动态库，不依赖台架用户态安装。

代价：

- NVIDIA 内核驱动、Intel DRM 内核驱动和相机内核驱动仍属于 OS/硬件边界。
- H.265 浏览器支持必须在实际驾驶端浏览器验收，不能只看编码器列表。
- NVENC 与 VAAPI 的属性名受 GStreamer 版本约束，Ubuntu 22.04 包必须实测。

## Bench evidence

- 4 路 720p30 自适应链路最终为 NVENC H.264，车端每路 28.96--29.39 fps。
- Chrome H.264 四轨持续解码为 30 fps，RTP `packetsLost=0`，估算端到端延迟
  8.86--13.39 ms。
- 注入 NVENC 故障后，VAAPI H.264 每路 29.48--29.71 fps；浏览器每路累计
  756--757 帧，丢帧 2--5，估算延迟 10.72--28.73 ms。
- 当前台架只有一个物理 V4L2 相机；四路负载由一台真实相机和三路原生 testsrc
  构成，仍需在四台真实相机接入后复验 USB/采集总线瓶颈。
