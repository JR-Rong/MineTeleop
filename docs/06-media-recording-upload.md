# 视频、录像与上传设计

## 目标

满足两个不同目标：

- 实时遥操：低延迟、低流量、可降级。
- 录像归档：原分辨率、可追溯、可上传。

这两个目标不能使用同一条无差别 pipeline。实时流和录像流必须从采集后分支，参数独立。

## 实时视频

### 默认 profile

```yaml
realtime_profile:
  codec: h265
  encoder: auto
  width: 1280
  height: 720
  fps: 30
  bitrate_kbps: 3000
  keyframe_interval_frames: 30
  latency_mode: low
```

### 编码策略

优先级：

1. 浏览器支持的首选 codec：NVENC，然后 Intel VAAPI。
2. 浏览器支持的 fallback codec：NVENC，然后 Intel VAAPI。
3. 两种硬件后端都失败时显式报错，不静默切换 CPU 编码。

H.265/HEVC 是配置首选，用来降低同等画质下的码率；H.264 仍是兼容性
fallback。浏览器先通过 WebRTC 接收能力上报 codec，车端只会 offer 浏览器明确
支持的 H.265，否则直接选择 H.264。

浏览器声明 H.265 并不等价于能同时稳定解码所有相机轨道。驾驶端每秒读取
`RTCPeerConnection.getStats()`；H.265 任一路连续 3 次低于 20 fps 时，通过已认证
信令请求 H.264。车端此时跳过同 codec 的 VAAPI 重试，直接按 NVENC、VAAPI 顺序
重新协商 H.264。编码器本身故障时仍先切换到同 codec 的 VAAPI。

需要单独确认 WebRTC H.264 profile 协商。当前硬件验证看到 VAAPI 支持 H.264 High profile，但部分 WebRTC 端默认偏好 constrained-baseline。首版实现必须在 SDP 协商、编码器 profile/level 和驾驶端解码能力之间做一致性验证，避免编码成功但驾驶端无法接收或解码。
本地 SDP 校验只接受 `a=rtpmap` 明确声明为 H264 的 payload type 对应
`a=fmtp` 中的 `profile-level-id`，避免把 VP8/其它 codec 的同名参数误当成
H.264 能力。

### 低延迟原则

- 关闭或减少 B 帧。
- GOP 不宜过长。
- 编码器使用 low-latency preset。
- 实时队列短。
- 拥塞时丢旧帧。
- 使用 UDP 优先的 WebRTC 路径。
- TURN 兜底节点必须支持 UDP。

实现保留实时 profile 的 `keyframe_interval_frames`，NVENC 关闭 B 帧并启用
zero-latency/CBR，VAAPI 使用硬件低延迟属性。每路 capture/encode 前都有短的
leaky queue，积压时丢弃旧帧。

## 多路相机

相机配置示例：

```yaml
cameras:
  - id: front
    enabled: true
    backend: auto
    device: /dev/video0
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265
  - id: rear
    enabled: true
    backend: auto
    device: /dev/video1
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265
```

每路相机必须能独立启停。单路故障不应导致全部视频中断。

`backend: auto` 是默认兼容路径，普通 V4L2 相机仍必须提供 native MJPEG，现有
Aravis/Basler、MVS 和 testsrc 路径也不改变。CCG2-8M 使用显式
`backend: ccg2`：采集端向驱动请求/校验其报告的 YUYV，同时按实测的 UYVY 内存
顺序逐行去除 stride padding，再以 `video/x-raw,format=UYVY` 直接送入 GStreamer 的
`videoconvert`/`videoscale` 和现有硬件编码器。仅当 `capture_fps` 与 realtime profile
输出 FPS 不同时，这条 raw pipeline 才在输出 caps 前插入 `videorate`；相同时不插入，
legacy MJPEG pipeline 仍沿用 realtime profile 的 input caps 且不插入 `videorate`。
CCG2 路径不先转成 JPEG，避免双路 `1920x1080@30` 多一次 CPU JPEG 编解码。
配置必须使用 ccg2-support 按 XDMA channel index 建立的稳定链接
`/dev/ccg2-channel-0` 至 `/dev/ccg2-channel-7`，不能依赖可能随枚举顺序改变的
`/dev/videoN`。

V4L2 协商得到的宽高、`bytesperline` 和 `bytesused` 是应用读取 buffer 的依据；当前
应用可见尺寸是 `1920x1080`。板卡状态工具中的 `1920x1536` input status 仅描述板端
输入链路，不能据此把 1536 行送入 pipeline。CCG2 内核驱动安装、设备节点创建和
板卡初始化都属于部署前置条件，不由媒体 runtime 代办。CCG2 启动时还会校验
`VIDIOC_S_PARM` 返回的 `timeperframe`：分子/分母必须有效，且有理数必须精确等于
`1 / capture_fps`，不能把驱动静默降帧当成已满足配置。

CCG2 的 Ubuntu 22.04 实机基线使用 Intel VAAPI 优先、NVENC 备用。该机的 GStreamer
1.20.3 配合 RTX 2000 Ada/595.84 驱动时，NVCodec 即使不传新版属性仍在启动阶段报告
`Selected preset not supported`，而同一 raw 帧已由 `vaapih264enc` 成功编码。因此
`configs/vehicle-agent.ccg2-8m.yaml` 不沿用通用配置的 NVENC 优先顺序。NVENC stage 在
1.20 不写 `preset`/`tune`，1.22 起才写 `preset=p1`，1.24 起才同时写
`tune=ultra-low-latency`；升级到 GStreamer 1.24+ 并重新完成实机编码验收后，才能把
该目标机改回 NVENC 优先。

## 录像

### 默认 profile

```yaml
record_profiles:
  record_source_h265:
    codec: h265
    encoder: reuse_realtime
    width: source
    height: source
    fps: source
    bitrate_kbps: 8000
    segment_seconds: 60
    container: mp4
```

说明：

- `source` 表示复用实时 pipeline 的实际分辨率和帧率。
- `tee` 位于硬件编码器之后；同一批 H.264/H.265 access unit 一路进入 RTP，
  另一路经 parser 直接交给 `splitmuxsink/mp4mux`。
- 录像不再启动 FFmpeg，也不再执行 `libx264` 二次编码。

### 容量规划

默认 4 路相机、每路录像 8 Mbps 时，录像产生速率约为 32 Mbps，约 14 GB/小时。若上传限速配置为 5 Mbps，长时间运行时上传必然追不上录像产生速率，最终触发磁盘水位保护。

因此首版必须明确：

- 目标本地保留时长。
- 车端磁盘可用容量。
- 每路录像码率和片段大小。
- 典型和最差 5G 上行带宽。
- 上传追不上时的处理策略：降录像码率、暂停上传以保护实时链路、扩大磁盘、只删除已上传文件、或在明确告警后接受未上传片段丢弃。

媒体进程每秒检查录像文件系统。可用空间低于 `min_free_gb` 时暂停录像分支的
输入，实时 WebRTC 和独立控制安全路径继续运行；空间恢复后自动恢复录像。
只有可用空间同时低于 `delete_uploaded_when_below_free_gb` 时才从最旧的
`upload_state=uploaded` 片段开始清理。`delete_unuploaded_when_below_free_gb=false`
是默认保护边界；只有显式设为 `true` 才允许删除未上传片段。每次清理、暂停、恢复
或空间检查失败都会输出带删除数量和 `safety_action` 的结构化诊断。

### 文件组织

建议：

```text
/var/lib/mine-teleop/recordings/
  vehicle-001/
    session-20260624-001/
      front/
        20260624T101500Z_front_000001.mp4
        20260624T101500Z_front_000001.json
      rear/
      left/
      right/
```

元数据示例：

```json
{
  "vehicle_id": "vehicle-001",
  "session_id": "session-20260624-001",
  "camera_id": "front",
  "segment_id": "20260624T101500Z_front_000001",
  "started_at": "2026-06-24T10:15:00.000Z",
  "ended_at": "2026-06-24T10:16:00.000Z",
  "timing_source": "splitmux_running_time",
  "codec": "h264",
  "encoder": "vaapi",
  "video_file": "20260624T101500Z_front_000001.mp4",
  "file_size_bytes": 64000000,
  "video_sha256": "50d858e0985ecc7f60418aaf0cc5ab587f42c2570a884095a9e8ccacd0f6545c",
  "upload_state": "pending"
}
```

正常运行时，sidecar 在 `splitmuxsink-fragment-closed` 到达时立即原子写入，片段
起止时间来自 pipeline running-time，因此已完成片段无需等到整条媒体流停止即可被
上传器发现。进程启动或停流时发现“有 MP4、无 sidecar”的片段，只会生成
`upload_state=quarantined`、时间和 codec/encoder 均未确认的恢复元数据；这类片段
不会被上传器当作 `pending` 自动归档，需人工确认。

## 上传队列

当前原生实现只支持 `upload.backend=local_archive`，按单个已完成片段立即调度，
因此启用上传时必须配置 `trigger_segments: 1`、`trigger_network_idle: false`。
S3、预签名 URL、累计字节/定时/网络空闲触发尚未实现，配置为其它 backend 会在
启动配置检查中明确失败，不能静默退回本地归档。

`vehicle-uploader --service-mode` 反复扫描 `recording.root_dir` 中
`upload_state=pending` 的 sidecar。每个片段在复制前必须提供 64 位
`video_sha256`，上传器先验证当前源文件仍等于录像时哈希，再验证原子临时目标；
哈希不一致的文件不会进入归档。sidecar 只能引用同目录的普通非符号链接视频文件，
拒绝绝对路径和 `..` 目录逃逸。

单个损坏 sidecar 会进入由 `retry_initial_seconds` 到 `retry_max_seconds` 控制的
指数退避，扫描继续处理后续健康片段；服务循环对 `failed`、`retry_wait` 和 `idle`
都执行有界休眠，避免紧密失败循环。退避状态只存在于当前上传进程内，进程重启后
会重新尝试；源 sidecar 成功后原子标为 `uploaded`。当 `upload.enabled=false`
时，入口在构造或扫描上传器之前返回 `disabled`，录像与 sidecar 仍保留。

### 上传状态

当前持久化到 sidecar 的状态：

- `pending`
- `uploaded`

`failed`、`retry_wait`、`idle` 和 `disabled` 是单次处理结果，不会伪装成已持久化
远端队列状态。`quarantined` 属于录像恢复状态，默认不参与上传。

不增加打包状态。已编码视频再次 zip/tar 通常不能显著省流量，还会增加 CPU 和磁盘双写；省流量应通过编码 profile、码率、分辨率、保留周期和上传调度控制。

## 流量控制

实时流优先级最高。

建议：

- 上传限速默认开启。
- 上传任务可暂停。
- 车端检测 5G 网络质量差时暂停上传。
- 实时流码率可动态降低。
- 上传限速必须与录像产生速率一起评估。若限速长期低于产生速率，系统必须进入明确降级或告警状态，而不是无限堆积队列。

本地参考实现提供上传网络质量策略：基于连接状态、RTT、抖动、丢包率和
上行带宽样本输出暂停/恢复决策，调用方可用该 reason 驱动上传队列
`pause()`；真实 5G modem/链路样本采集仍需在车端环境接入。
部署模板还会把 uploader 降为低 CPU/IO 调度优先级：systemd 使用较低
`CPUWeight`/`IOWeight` 和 idle IO 调度，容器模板使用低于 control/media 的
`cpu_shares`，避免大文件上传与实时控制和媒体 pipeline 争抢调度优先级。

本地参考实现同时提供实时流码率自适应策略：RTT 或丢包超阈值时按比例
下调且不低于下限，网络恢复后逐步上调到目标码率；并提供实时媒体 runtime
controller，把允许的实时 profile 码率更新转成命名 encoder 的 `bitrate`
property update，并通过 GStreamer pipeline property setter 绑定到命名元素。
弱网需要进一步降级时，runtime 也提供 profile 级策略，可在已声明的实时
profile 之间按顺序从 720p30 下切到 480p15 等低帧率/低分辨率档位，并在
网络恢复后上切；profile 切换通过显式 pipeline hook 成功后才更新活动状态。
真实 GStreamer/WebRTC 主循环仍需在目标 Ubuntu 工控机做端到端验证。

## 4 路编码压力验证

在工控机上建议进一步验证：

- 4 路 720p30 H.264 VAAPI 实时编码。
- 同时 4 路原分辨率录像编码。
- 同时开启 WebRTC 发送和本地写盘。
- 记录 CPU、GPU、内存、磁盘 IO、温度。

单路 Docker 测试已证明硬件能力存在，但还没有证明完整 4 路并发足够。
