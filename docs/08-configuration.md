# 配置体系

## 目标

系统必须尽量配置化，避免将相机数量、码率、路径、云端地址、安全阈值写死。

## 配置文件建议

使用 YAML 或 TOML。首版推荐 YAML，便于人工编辑和嵌套结构表达。

建议路径：

```text
/etc/mine-teleop/vehicle-agent.yaml
/etc/mine-teleop/driver-console.yaml
```

开发环境可使用：

```text
config/vehicle-agent.dev.yaml
config/driver-console.dev.yaml
```

## 车端配置示例

```yaml
vehicle:
  id: vehicle-001
  name: mine-truck-001

cloud:
  signaling_url: wss://teleop.example.com/signaling
  auth_url: https://teleop.example.com/auth
  device_cert: /etc/mine-teleop/certs/vehicle.crt
  device_key: /etc/mine-teleop/certs/vehicle.key

ice:
  stun_servers:
    - stun:turn.example.com:3478
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: vehicle-001
      credential_file: /etc/mine-teleop/turn.secret

control:
  rate_hz: 20
  freshness_mode: local_receive_interval_and_seq
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  timeout_action:
    throttle: 0.0
    deceleration_profile:
      - after_ms: 0
        brake: 0.3
      - after_ms: 500
        brake: 0.6
      - after_ms: 1500
        brake: vehicle_defined_max_safe
    gear_before_stopped: hold_current_or_vehicle_safe_mode
    stopped_action:
      gear: N
      apply_parking_brake: true
  estop:
    latch: true
    reset_requires_local_confirmation: true
  time_sync:
    minimum: ntp
    ptp_required_for_multicamera_sync: evaluate_later

media:
  realtime_profiles:
    realtime_720p:
      codec: h264
      encoder: vaapi
      width: 1280
      height: 720
      fps: 30
      bitrate_kbps: 3000
      keyframe_interval_frames: 30
      low_latency: true
  record_profiles:
    record_source_h264:
      codec: h264
      encoder: vaapi
      width: source
      height: source
      fps: source
      bitrate_kbps: 8000
      segment_seconds: 60

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

recording:
  root_dir: /var/lib/mine-teleop/recordings
  retention_target_hours: 8
  capacity_plan_required: true
  min_free_gb: 50
  delete_uploaded_when_below_free_gb: 30
  delete_unuploaded_when_below_free_gb: false

upload:
  enabled: true
  backend: s3
  max_bandwidth_mbps: 5
  trigger_segments: 20
  direct_file_upload: true
  presigned_url_refresh_margin_seconds: 300
  retry_initial_seconds: 10
  retry_max_seconds: 600

vehicle_adapter:
  type: mock
```

## 驾驶端配置示例

```yaml
driver:
  id: driver-console-001

cloud:
  auth_url: https://teleop.example.com/auth
  signaling_url: wss://teleop.example.com/signaling

ui:
  default_layout: grid_4
  show_debug_overlay: true

control:
  rate_hz: 20
  keyboard:
    steering_left: A
    steering_right: D
    throttle: W
    brake: S
    estop: E
```

## 配置校验

启动时必须校验：

- 必填字段存在。
- 相机 ID 不重复。
- 启用相机至少 1 路。
- fps、码率、分辨率在合法范围。
- 文件路径可读或可写。
- 证书文件存在。
- TURN URL 格式合法。
- 控制超时大于命令周期。
- 安全停车制动曲线存在且不是单步全力制动。
- `max_command_gap_ms`、`degraded_timeout_ms`、`control_timeout_ms` 按递增关系配置。
- 急停锁存和复位策略已配置。
- 时间同步策略已配置。
- 录像容量规划已配置，上传限速低于录像产生速率时必须给出保留或降级策略。

## 热更新

首版不建议做完整热更新。

可考虑热更新：

- 日志级别。
- 实时码率。
- 上传限速。
- 上传暂停/恢复。

不建议热更新：

- 车辆 ID。
- 证书路径。
- 车辆控制适配器。
- 相机设备列表。
- 安全停车策略。
