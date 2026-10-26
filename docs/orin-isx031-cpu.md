# Orin Nano / ISX031 CPU H.264 台架

这条路径在原生主程序内采集 UYVY、缩放并通过 `x264enc` 编码 H.264，再交给现有 WebRTC 管线。`backend: v4l2_uyvy` 请求真正的 UYVY；既有 `ccg2` 后端仍保留“驱动报告 YUYV、实际字节为 UYVY”的专用约定。

CPU 编码须同时显式设置 `preferred_encoder: cpu`（或 CPU fallback）和 `require_hardware_encoder: false`。目前 CPU 只支持 H.264；不会自动放宽既有硬件编码要求，也不会把 CPU 加入 H.265 候选。编码器使用 ultrafast、zerolatency、4 个线程、无 B 帧/前瞻，关键帧间隔沿用运行时的视频恢复预算。

## 运行依赖

Ubuntu ARM64 / Jetson Linux 台架：

```sh
sudo apt-get install gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-nice v4l-utils psmisc
for plugin in x264enc h264parse rtph264pay webrtcbin nicesrc nicesink \
  dtlssrtpenc dtlssrtpdec sctpenc sctpdec; do
  gst-inspect-1.0 "$plugin" >/dev/null || exit 1
done
```

仅存在视频节点或安装了 NVIDIA GStreamer 插件不能证明硬件编码可用；以 `mine-teleop media-probe` 和实际编码结果为准。

## HCK-C1202 SerDes 初始化

已确认台架实物为 ISX031，原生输出模式是 **1920×1536 UYVY / 30 fps**。不能使用旧 AR0233/RG12 配置。仅对 `/dev/videoN` 设置格式不足以完成本载板初始化；串行器和解串器 pad 的媒体格式也必须下发。依据 [HCK 官方说明](https://www.hckdigi.com/doc/mi-12x4/05-Accessory/h4xgmsl2.html)，SerDes 使用 `YUYV8_1X16`，视频节点使用 `UYVY`。

先停止相机占用，再查看实际拓扑：

```sh
media-ctl -d /dev/media0 -p
```

本次布线的映射如下；重新接线或更换设备树后必须重新确认，不能假定 video 编号恒定。

| 视频节点 | 传感器 | 串行器 | 解串器 |
| --- | --- | --- | --- |
| /dev/video0 | nv_cam 20-001a | ser_5_ch_0 | des_1_ch_1 |
| /dev/video1 | nv_cam 21-001a | ser_0_ch_0 | des_0_ch_0 |

```sh
scripts/deploy/setup_orin_isx031.sh /dev/video0 /dev/media0 ser_5_ch_0 des_1_ch_1
scripts/deploy/setup_orin_isx031.sh /dev/video1 /dev/media0 ser_0_ch_0 des_0_ch_0
```

安装包中的脚本位于 `share/mine-teleop/scripts/`，标准 vehicle bundle 中位于 `scripts/`。脚本检查设备、工具、实体与占用，选择 `sensor_mode=0`、`bypass_mode=0`，只设置运行时格式；不会改设备树、开机配置或 CAN。它应在设备重启后、启动媒体服务之前执行，而不是在控制线程或每帧路径中调用。

原先漏设 SerDes 时，两路反复出现 VI timeout、`CHANSEL_NOMATCH`、零时间戳或错误 buffer；补齐设置后原生模式正常。1080 模式仍出现错误 buffer，因此这里在 1536 原生模式采集，再由用户态等比例缩放至 960×768 编码。

## 首帧前 WebRTC 协商

PR24 会在首帧前生成 offer。在台架 GStreamer 1.20.3 上，仅提供 codec/payload caps 时，SDP 会声明一个与实际 payloader 不同的 SSRC；浏览器虽然收到并解码 RTP，却没有可呈现的视频轨道。本适配为每次管线、每个视频 lane 分配非零且不同的 SSRC，并同时设置 payloader 和固定 RTP caps。协商前拒绝缺失 SSRC 的视频 lane，保留原来的无帧协商与 NACK/RTX。回归覆盖八路 H.264/H.265 的首个 offer 身份及无 SSRC 拒绝；真实浏览器核验呈现帧增长。

## 台架配置与验证顺序

- `configs/vehicle-agent.orin-cpu-bench.yaml`：单路 testsrc → 1280×720 / 30 fps / 2500 kbps CPU H.264。
- `configs/vehicle-agent.orin-isx031-bench.yaml`：两路 1920×1536 UYVY → 每路 960×768 / 30 fps / 2500 kbps CPU H.264。

两个配置都使用内置 mock、最大速度 0，禁用录制/上传；已有鉴权、校时、控制期限和关键视频健康门禁保留。它们是台架示例，不是实车配置。主程序原生采集保留尺寸/stride/bytesused 检查、错误 buffer 拒绝、序号与时间戳诊断和有界重开；不同输出尺寸不会改变相机采集尺寸。

先运行 `media-probe` 与配置 preflight，再使用同一信令和驾驶端验证测试图，然后切换真实相机配置。台架所有角色共享回环 IP 时，20 Hz polling/heartbeat 会超过默认 600/min 配额；可仅在隔离信令实例使用 `--api-rate-limit-requests 12000`。不要为此修改产品默认安全配置。

依次记录：实际编码后端、ICE 候选、浏览器解码帧增长、FPS、丢包/丢帧/冻结、采集错误/重开、进程 CPU/内存和温度。静态截图不能代替连续解码证据；估计延迟不能替代真实采集到显示延迟测量。

## 构建与验收

基线为 PR24 `eb0ef621c8212ba653b4821cacc944ac879ebf8c`，PR25 叠加本适配。使用本地 `orin` ARM64 Ubuntu 22.04 容器，启用 `MINE_TELEOP_BUILD_TESTS`、`MINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS`，关闭不需要的厂商 SDK bridge；`cmake --install` 同时安装两个示例配置和初始化脚本。

2026-10-26 本次验证（[机器可读摘要](evidence/orin-isx031-cpu-20261026.json)）：

- `orin` 容器 ARM64 构建通过；最终受影响的 5 组回归通过。64 组完整 CTest 首轮通过 62 组，校时 suite 出现两次时序抖动后单独复测通过；累计 63 组取得通过结果。剩余 `mine-teleop-product-direct-integration-tests` 报 `DC not qualified`，未改动 PR24 基线同样失败，不能声称全量回归全绿。
- 台架最终原生核心测试 **86/86**，八路无首帧 WebRTC / SSRC / NACK / RTX 回归通过；media-probe 确认 CPU `x264enc` 可用，真实相机 preflight 通过。
- 测试图在浏览器自动呈现 1280×720。40 秒稳定采样新增 1201 解码帧、1200 呈现帧，约 30 fps / 2.5 Mbps；无新增解码丢帧、丢包或冻结。
- 两路 ISX031 的稳定采样持续约 **270 秒**，各新增 **8100 解码帧**，约 30 fps / 2.5 Mbps；各呈现 7569 / 7556 帧（平均约 28 fps）。无新增解码丢帧、RTP 丢包、冻结、采集错误、重开或采集序号间断；内核日志无新增 VI timeout / CHANSEL_NOMATCH。
- 启动阶段保留观测：最大约 1.9 秒采集/编码延迟、每路约 100 个 raw backlog/drop 计数、每路 4 个 stale AU 丢弃；稳态无新增 stale 丢弃。浏览器初始解码丢帧为 1 / 3，随后不增长。不能将稳态窗口写成启动全过程零丢帧。
- 真实相机接收到解码的估计总延迟约 75 ms；这是诊断估计，不是光子到显示测量。20 秒温度采样约 44°C，内存约 2.2 GB（整机），采样时媒体进程 RSS 约 169 MiB。

测试仅使用隔离信令、内置 mock、零速度，CAN 保持 DOWN，没有 VCU 握手或真实驾驶输入。270 秒观察后还保留了 CCG2 错误码兼容性修正；最终二进制另做真实相机重连检查，摘要分别记录二进制身份。短台架测试不代表长稳、实车控制或 210 分钟验收。
