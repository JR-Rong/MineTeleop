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

### 上传状态

状态建议：

- `pending`
- `packing`
- `uploading`
- `uploaded`
- `failed`
- `retry_wait`

上传队列需要持久化，避免车端重启后丢失状态。

## 流量控制

实时流优先级最高。

建议：

- 上传限速默认开启。
- 上传任务可暂停。
- 车端检测 5G 网络质量差时暂停上传。
- 实时流码率可动态降低。

## 4 路编码压力验证

在工控机上建议进一步验证：

- 4 路 720p30 H.264 VAAPI 实时编码。
- 同时 4 路原分辨率录像编码。
- 同时开启 WebRTC 发送和本地写盘。
- 记录 CPU、GPU、内存、磁盘 IO、温度。

单路 Docker 测试已证明硬件能力存在，但还没有证明完整 4 路并发足够。

