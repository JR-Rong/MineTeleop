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
  codec: h264
  encoder: vaapi
  width: 1280
  height: 720
  fps: 30
  bitrate_kbps: 3000
  keyframe_interval_frames: 30
  latency_mode: low
```

### 编码策略

优先级：

1. `h264_vaapi` 或 GStreamer VAAPI/QSV H.264。
2. CPU `x264` ultra-fast/zerolatency。
3. 降帧或降分辨率。

H.264 是首版推荐，因为驾驶端兼容性最好。H.265/HEVC 码率更低，但解码兼容性、浏览器支持和部分硬件支持更复杂，不建议第一版实时流直接强依赖。

需要单独确认 WebRTC H.264 profile 协商。当前硬件验证看到 VAAPI 支持 H.264 High profile，但部分 WebRTC 端默认偏好 constrained-baseline。首版实现必须在 SDP 协商、编码器 profile/level 和驾驶端解码能力之间做一致性验证，避免编码成功但驾驶端无法接收或解码。

### 低延迟原则

- 关闭或减少 B 帧。
- GOP 不宜过长。
- 编码器使用 low-latency preset。
- 实时队列短。
- 拥塞时丢旧帧。
- 使用 UDP 优先的 WebRTC 路径。
- TURN 兜底节点必须支持 UDP。

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
    record_profile: record_source_h264
  - id: rear
    enabled: true
    device: /dev/video1
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h264
```

每路相机必须能独立启停。单路故障不应导致全部视频中断。

## 录像

### 默认 profile

```yaml
record_profiles:
  record_source_h264:
    codec: h264
    encoder: vaapi
    width: source
    height: source
    fps: source
    bitrate_kbps: 8000
    segment_seconds: 60
    container: mp4
```

说明：

- `source` 表示使用采集原始分辨率或帧率。
- 如果磁盘或编码压力过高，可降低录像帧率或码率。
- 录像和实时流可以分别使用不同 encoder instance。

### 容量规划

默认 4 路相机、每路录像 8 Mbps 时，录像产生速率约为 32 Mbps，约 14 GB/小时。若上传限速配置为 5 Mbps，长时间运行时上传必然追不上录像产生速率，最终触发磁盘水位保护。

因此首版必须明确：

- 目标本地保留时长。
- 车端磁盘可用容量。
- 每路录像码率和片段大小。
- 典型和最差 5G 上行带宽。
- 上传追不上时的处理策略：降录像码率、暂停上传以保护实时链路、扩大磁盘、只删除已上传文件、或在明确告警后接受未上传片段丢弃。

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

## 4 路编码压力验证

在工控机上建议进一步验证：

- 4 路 720p30 H.264 VAAPI 实时编码。
- 同时 4 路原分辨率录像编码。
- 同时开启 WebRTC 发送和本地写盘。
- 记录 CPU、GPU、内存、磁盘 IO、温度。

单路 Docker 测试已证明硬件能力存在，但还没有证明完整 4 路并发足够。
