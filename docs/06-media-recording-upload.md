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
    device: /dev/video0
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265
  - id: rear
    enabled: true
    device: /dev/video1
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265
```

每路相机必须能独立启停。单路故障不应导致全部视频中断。

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

本地水位策略默认只删除 `upload_state=uploaded` 的片段；若现场配置显式允许
删除未上传片段，返回动作必须标记为 `deleted_unuploaded_segments`，并带
`explicit unuploaded deletion policy` 原因，避免运维误认为只是普通已上传清理。

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
  "started_at": "2026-06-24T10:15:00Z",
  "ended_at": "2026-06-24T10:16:00Z",
  "codec": "h264",
  "encoder": "vaapi",
  "width": 1920,
  "height": 1080,
  "fps": 30,
  "file_size_bytes": 64000000,
  "video_sha256": "4b8c...",
  "upload_state": "pending"
}
```

## 上传队列

### 触发策略

可配置：

- 每 N 个视频片段上传。
- 每累计 N MB 上传。
- 每 N 分钟上传。
- 网络空闲时上传。

触发策略只决定何时调度上传；默认上传单位是单个视频片段和对应 sidecar 元数据，不做 zip/tar 打包。
本地 uploader 会接入数量、累计字节、等待时间和网络空闲四类触发条件；
`network_idle` 由调用方根据链路策略传入，真实 5G modem/网络空闲采样仍需车端环境接入。

本地 recorder/uploader 闭环会同时申请 video 和 metadata 两类上传凭证，
队列持久化两个对象路径，以及视频文件和 sidecar 元数据文件的 SHA-256
校验值和 `enqueued_at_ms` 入队时间；上传调度的时间窗口按最早 pending
片段的入队时间计算。上传成功后分别登记视频片段和 sidecar 元数据，并将源
sidecar 与归档 sidecar 的 `upload_state` 原子更新为 `uploaded`。
`upload.backend=s3` 时，车端会把视频和 sidecar 直接 HTTP PUT 到签发的
URL；`local_archive` 后端只用于本地开发归档。
当 `upload.enabled=false` 时，本地录像和 sidecar 写入保持可用，但 uploader
不会申请凭证、入队、扫描 pending sidecar 或执行上传；单次调度返回
`disabled`。
部署入口必须使用 `vehicle-uploader --service-mode`，从
`recording.root_dir` 扫描已有 pending sidecar，并把队列状态和本地归档写入
独立 work dir；默认不带 `--service-mode` 的入口只用于本地闭环 demo，会创建
演示片段后退出。目标主机 smoke 可加 `--process-once --json`，把单次调度结果
输出为 `vehicle_uploader_process_once` JSONL 证据，便于归档报告区分
`uploaded`、`idle`、`wait`、`disabled` 和 `failed`。
本地归档适配器会校验对象路径必须落在归档根目录内，拒绝 `../` 或绝对路径
造成的目录逃逸。

### 上传状态

状态建议：

- `pending`
- `uploading`
- `uploaded`
- `failed`
- `retry_wait`
- `credential_refresh`

上传队列需要持久化，避免车端重启后丢失状态。

使用预签名 URL 时，队列持久化的是片段 ID、对象路径、校验信息和上传状态。预签名 URL 本身可能过期，车端在 `uploading` 前应检查剩余有效期，过期或低于安全余量时进入 `credential_refresh`，重新向云端申请同一对象路径的上传凭证。

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
