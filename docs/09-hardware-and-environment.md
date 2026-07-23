# 硬件与环境排查结论

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

## 工控机环境

已知信息：

- OS：Ubuntu 22.04.5 LTS。
- Kernel：6.8.0-85-generic。
- 架构：x86_64。
- GPU：Intel AlderLake-S GT1。
- 驱动：`i915`。
- DRM render node：`/dev/dri/renderD128`。

## NVIDIA 结论

当前没有证据表明工控机可用 NVIDIA 编码。

证据：

- PCI 列表没有 NVIDIA 设备。
- `nvidia-smi` 不存在或不可用。
- GStreamer 未检测到 `nvh264enc` / `nvh265enc`。

设计影响：

- 当前项目不能依赖 NVENC。
- 配置可保留 `nvenc` 后端枚举，但默认不可用。

## Intel 结论

Intel VAAPI 硬件编码可用。

证据：

- `vainfo` 在 Docker 容器内成功打开 Intel iHD 驱动。
- 支持 `VAProfileH264High : VAEntrypointEncSliceLP`。
- 支持 `VAProfileHEVCMain : VAEntrypointEncSliceLP`。
- Docker 内 FFmpeg 使用 `h264_vaapi` 成功编码 1280x720 30 fps 测试视频。
- `ffprobe` 确认输出：
  - `codec_name=h264`
  - `width=1280`
  - `height=720`
  - `avg_frame_rate=30/1`
  - `bit_rate=4329553`

## 宿主机 FFmpeg 问题

宿主机 `/usr/local/bin/ffmpeg` 不是 Ubuntu apt 版本，而是较新的 2025 master 构建。

它调用了系统 `libva2 2.14.0` 中不存在的符号：

```text
vaMapBuffer2 missing
```

结论：

- 这不是 Intel 硬件不可用。
- 这是 FFmpeg 和 libva ABI 不匹配。
- 不应继续用 `/usr/local/bin/ffmpeg` 作为硬编能力判断依据。

## 宿主机 apt/ROS 状态

宿主机 apt 处于 broken 状态。

已见问题：

- `python3-catkin-pkg` 与 `python3-catkin-pkg-modules` 文件冲突。
- 多个 ROS Humble 包处于 `iU` 未完成配置状态。
- `apt install ffmpeg vainfo intel-media-va-driver` 会被 broken dependency 阻断。

结论：

- 不建议为了验证硬编而强修宿主机 apt。
- 当前更安全的验证方式是 Docker 容器挂载 `/dev/dri`。

## Docker 验证命令

已使用的验证思路：

```bash
sudo docker run --rm \
  --device /dev/dri/renderD128 \
  --device /dev/dri/card1 \
  -v /tmp:/out \
  ubuntu:22.04 \
  bash -lc '
    apt-get update &&
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ffmpeg vainfo intel-media-va-driver &&
    vainfo --display drm --device /dev/dri/renderD128 || true &&
    ffmpeg -hide_banner \
      -vaapi_device /dev/dri/renderD128 \
      -f lavfi -i testsrc2=size=1280x720:rate=30 \
      -t 5 \
      -vf "format=nv12,hwupload" \
      -c:v h264_vaapi -b:v 4M \
      -y /out/vaapi-h264-docker.mp4 &&
    ffprobe -hide_banner \
      -select_streams v:0 \
      -show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate \
      -of default=nw=1 \
      /out/vaapi-h264-docker.mp4
  '
```

## 下一步硬件验证

需要补测：

- 4 路 720p30 H.264 VAAPI 并发编码。
- 4 路实时流加 4 路录像流同时运行。
- H.264 profile/level 与 WebRTC SDP 协商兼容性，特别是 VAAPI High profile 与驾驶端 constrained-baseline 偏好之间的匹配。
- 长时间运行稳定性，例如 2 小时。
- CPU/GPU/内存/磁盘 IO/温度。
- GStreamer 是否能使用 Intel 硬编插件。

特别注意：

当前 GStreamer 检测中未发现 `vaapih264enc`、`qsvh264enc`、`vah264enc`。因此 FFmpeg 证明了 Intel 硬编能力，但不能自动证明 GStreamer 项目链路已经可用。

## 验证结果归档

目标工控机完成并发编码后，把每路 `ffprobe` 的 `key=value` 输出保存为文件，
再把 CPU/GPU/内存/磁盘/温度等采样保存为 JSON。然后用本仓库生成统一 JSONL
验收记录：

```bash
python3 vehicle-media-agent/vehicle_media_agent.py \
  --mode hardware-report \
  --scenario four-camera-realtime-720p30 \
  --ffprobe-output front-realtime-720p30=/tmp/front.ffprobe.txt \
  --ffprobe-output rear-realtime-720p30=/tmp/rear.ffprobe.txt \
  --ffprobe-output left-realtime-720p30=/tmp/left.ffprobe.txt \
  --ffprobe-output right-realtime-720p30=/tmp/right.ffprobe.txt \
  --metrics-json /tmp/mine-teleop-vaapi-metrics.json
```

报告第一行是 `hardware_encoding_validation` 汇总，后续包含每路
`hardware_encoding_lane` 和一条 `hardware_encoding_metrics`。任一路 codec、
分辨率、fps 或码率不符合场景期望时返回 2；全部通过时返回 0。
