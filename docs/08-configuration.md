# 配置体系

## 目标

系统必须尽量配置化，避免将相机数量、码率、路径、云端地址、安全阈值写死。

## 配置文件建议

使用 YAML 或 TOML。首版推荐 YAML，便于人工编辑和嵌套结构表达；本地参考实现的
`load_vehicle_config` 和 `load_driver_config` 会按 `.toml` 后缀读取 TOML。

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
      username: mine-teleop
      credential_mode: turn_rest
      static_auth_secret_file: /etc/mine-teleop/turn-static-auth.secret
      credential_ttl_seconds: 600

control:
  rate_hz: 20
  freshness_mode: local_receive_interval_and_seq
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  # 非 mock 车辆适配器必须提供该标定证据，且 control_timeout_ms 不能超过上限。
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
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
    realtime_480p15:
      codec: h264
      encoder: vaapi
      width: 854
      height: 480
      fps: 15
      bitrate_kbps: 1200
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

hardware:
  can:
    interface: can0
    bitrate: 500000
    restart_ms: 100
    probe_timeout_seconds: 3
  encoding:
    vaapi_render_device: /dev/dri/renderD128
    dri_card_device: /dev/dri/card1
    require_hardware_encoder: true
    gstreamer_hardware_plugins:
      - vaapih264enc
      - qsvh264enc
      - vah264enc
      - nvh264enc
    gstreamer_fallback_plugins:
      - x264enc
    ffmpeg_probe_output_dir: /tmp/mine-teleop-vaapi
    validation_duration_seconds: 5
  network:
    interface: wwan0

field_safety:
  commissioning_mode: bench
  max_speed_kph: 40
  require_can_feedback_before_control: true
  require_local_estop_reset: true
  require_time_sync: true

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
  trigger_network_idle: true
  direct_file_upload: true
  presigned_url_refresh_margin_seconds: 300
  retry_initial_seconds: 10
  retry_max_seconds: 600
  s3:
    endpoint_url: https://s3.us-west-2.amazonaws.com
    bucket: mine-teleop-recordings
    region: us-west-2
    access_key_id: AKIDEXAMPLE
    secret_access_key_file: /etc/mine-teleop/secrets/s3-secret-access-key

vehicle_adapter:
  type: mock
```

`cameras[].enabled` 必须写成 YAML/TOML boolean `true`/`false`，不能用带引号字符串；
禁用相机不应被误纳入设备 preflight 或媒体 pipeline。

`hardware.can.interface` 是车端 adapter、MinePilot CAN smoke 和目标主机验收计划共同使用的
SocketCAN 接口名；真实 adapter 配置中
`vehicle_adapter.integration.chassis_control.can_interface` 必须与它一致。`hardware.can.bitrate`
用于现场 CAN 口配置记录和部署命令，应用进程不会自动改 Linux netdev，需要现场用 `ip link`
按该值配置。

`hardware.encoding` 暴露硬件编码相关现场变量。`vaapi_render_device`、`dri_card_device` 会进入
preflight、`vainfo`、FFmpeg VAAPI probe 和硬编验收计划；`gstreamer_hardware_plugins` 与
`gstreamer_fallback_plugins` 会进入 `gst-inspect-1.0` 探测命令。工控机如果换了 DRI 节点、
编码插件或临时输出目录，只改这里。

`hardware.network.interface` 会进入弱网矩阵和目标主机验收脚本。`field_safety` 用来记录现场
安全链路的最低门禁：调试阶段、速度上限、是否必须先收到 CAN feedback、是否必须本地确认
急停复位、是否强制时间同步。它不替代现场安全员和物理急停，但会进入有效配置日志和验收记录。

上传限速必须是有限正数；上传触发数量、URL 刷新安全余量和重试退避时间
必须是正数；`retry_initial_seconds` 不能大于 `retry_max_seconds`。
`upload.enabled`、`upload.direct_file_upload` 与 `upload.trigger_network_idle`
必须写成 YAML/TOML boolean `true`/`false`，不能用带引号字符串。
当前本地参考实现只支持逐文件直接上传，因此 `upload.direct_file_upload`
必须保持 `true`；打包上传模式未实现时不能用 `false` 静默表达。
`upload.enabled=false` 只关闭上传侧效果；录像和 sidecar 仍会写入本地磁盘，
但不会申请上传凭证、入队、扫描 pending sidecar 或执行上传。
`delete_unuploaded_when_below_free_gb` 是破坏性开关，必须写成 YAML/TOML boolean
`true`/`false`，不能用带引号字符串。
`upload.backend=s3` 时必须配置 `upload.s3` 的 endpoint、bucket、region、
access key 和 secret。Secret 可以直接配置，也可以用
`secret_access_key_file` 指向只读凭据文件；运行时有效配置日志只记录
`configured`，不输出 secret 值或 secret 文件路径。

当前参考实现只支持 `vehicle_adapter.type=mock` 直接无外部依赖运行。配置为
`can` 或 `dynamic_library` 前必须先声明真实车辆接口契约，例如：

```yaml
vehicle_adapter:
  type: dynamic_library
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: required
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: /Volumes/SystemDisk/Workspace/ChassisControl
      header_path: /Volumes/SystemDisk/Workspace/ChassisControl/chassis_control.h
      can_common_header_path: /Volumes/SystemDisk/Workspace/ChassisControl/include/can/can_common.h
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      # 当前 ChassisControl 头文件暴露 C++ API，不能直接作为 Python ctypes C ABI 调用。
      abi: cplusplus
      requires_cpp_bridge: true
    minepilot:
      source_root: /Volumes/SystemDisk/Workspace/MinePilot
      can_common_header_path: /Volumes/SystemDisk/Workspace/MinePilot/include/can/can_common.h
      can_message_header_path: /Volumes/SystemDisk/Workspace/MinePilot/include/can/can_message.h
      can_db_header_path: /Volumes/SystemDisk/Workspace/MinePilot/include/can_db.h
      can_receiver_header_path: /Volumes/SystemDisk/Workspace/MinePilot/include/can_receiver.h
      can_sender_header_path: /Volumes/SystemDisk/Workspace/MinePilot/include/can_sender.h
      can_db_source_path: /Volumes/SystemDisk/Workspace/MinePilot/src/can_db.cpp
      can_receiver_source_path: /Volumes/SystemDisk/Workspace/MinePilot/src/can_receiver.cpp
      can_sender_source_path: /Volumes/SystemDisk/Workspace/MinePilot/src/can_sender.cpp
```

如果缺少契约，配置加载必须失败；非 `mock` 适配器还必须配置
`control.timeout_calibration.max_control_timeout_ms` 和 `evidence`，并确保
`control_timeout_ms` 不超过标定上限。`can` 和 `dynamic_library` 在完成
timeout calibration 后也必须声明 `integration.chassis_control`，否则配置加载阶段
就失败，不能等到 runtime 再暴露缺 bridge/source 绑定。

`can` 与 `dynamic_library` 的 ChassisControl 集成会校验本地源码、头文件、CAN C ABI 头文件、
CMake target、CAN interface 和 MinePilot 低层 CAN、`can_db`/receiver/sender 头文件与源码来源，并生成控制命令到
ChassisControl `VehicleState` 的调用意图。由于当前 ChassisControl 暴露的是 C++
函数和结构体，运行时接入必须先提供 C++ bridge/C shim；在 bridge 可用前进程启动
仍必须失败，避免误以为已经接入真实底盘。

bridge 编译完成后，运行时配置应切换为 C shim ABI，并指向实际产物：

```yaml
vehicle_adapter:
  type: can  # dynamic_library 也可使用同一个 c_shim bridge
  integration:
    chassis_control:
      abi: c_shim
      requires_cpp_bridge: false
      bridge_library_path: /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so
```

可用下面的脚本从开发配置生成目标车端配置，再按现场 cloud/TURN/证书/录像路径
做部署覆盖：

```bash
python3 scripts/render_chassis_vehicle_config.py \
  --base-config configs/vehicle-agent.dev.yaml \
  --output /etc/mine-teleop/vehicle-agent.yaml \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --can-interface can0 \
  --max-control-timeout-ms 900 \
  --calibration-evidence bench-brake-test-2026-06-24
```

脚本会注入 `abi: c_shim`、`requires_cpp_bridge: false`、MinePilot
`include/can/*.h`、`include/can_db.h`、`include/can_receiver.h`、
`include/can_sender.h` 和 `src/can_db.cpp`/receiver/sender 路径，并写入
实际链接的 `libchassis_control` 路径与 `control.timeout_calibration`。输出的 YAML 仍需在目标机通过 `vehicle-agent
--adapter-status` 验证 bridge 能被 runtime 打开。

仓库提供 `deployments/chassis-control-bridge/` 模板和
`mine_teleop_chassis_bridge.h` 稳定 ABI 头，导出 Python adapter 所需的
`mine_teleop_chassis_open`、`mine_teleop_chassis_apply_state`、
`mine_teleop_chassis_emergency_stop`、`mine_teleop_chassis_update_feedback`、
`mine_teleop_chassis_poll_feedback`、`mine_teleop_chassis_read_telemetry` 和
`mine_teleop_chassis_close`。该 bridge 会链接
ChassisControl `chassis_control` 动态库；MinePilot `include/can/can_common.h`、
`include/can/can_message.h` 和 `can_db`/receiver 头文件作为反馈解码接入来源保留在构建配置中，
`can_sender.h` 作为发送侧依赖一并记录和校验；bridge 前置检查还会确认
`src/can_db.cpp`、`src/can_receiver.cpp` 和 `src/can_sender.cpp` 存在，
避免目标主机发送/接收探针缺少源码。
运行时的 CAN 接收线程应把 MinePilot `DecodedCanData` 形态的最新数据交给
`ChassisControlFeedbackPump`；该泵会抽取握手、驻车、挡位、MCU/EPS/EHB 模式和车速快照，并调用 adapter `update_feedback`，最终进入
`mine_teleop_chassis_update_feedback`，供 ChassisControl arming 状态机和 telemetry
使用。

## 控制超时参数语义

- `max_command_gap_ms`：单次有效命令到达间隔上限，用于丢弃过旧命令、记录异常和提示链路抖动。
- `degraded_timeout_ms`：链路异常持续多久后进入降级控制，用于区分偶发丢包和连续抖动。
- `control_timeout_ms`：持续没有有效控制心跳多久后进入 `TIMEOUT_BRAKE`。

`control_timeout_ms` 不能只按网络体验调大。配置前必须结合车速上限、制动曲线、坡道/松散路面和矿区安全距离，反推允许的最大控制超时。本地参考实现会在非 `mock` 车辆适配器配置中要求 `timeout_calibration` 标定证据，并拒绝超过标定上限的 `control_timeout_ms`。

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
  estop_hold_ms: 500
  keyboard:
    steering_left: A
    steering_right: D
    throttle: W
    brake: S
    estop: E
  gamepad:
    enabled: true
    steering_axis: 0
    throttle_axis: 2
    brake_axis: 5
    axis_deadzone: 0.05
    throttle_inverted: true
    brake_inverted: true
    estop_button: 0
```

`ui.show_debug_overlay` 必须写成 YAML/TOML boolean `true`/`false`，不能用带引号
字符串，避免调试层在正式驾驶端被误启用或误关闭。
`control.gamepad` 的轴编号来自浏览器 Gamepad API；如果现场方向盘/踏板轴顺序不同，
只需要调整该配置，不需要改 Docker 镜像。

## 配置校验

启动时必须校验：

- 必填字段存在。
- 相机 ID 不重复。
- 启用相机至少 1 路。
- fps、码率、分辨率在合法范围。
- 文件路径可读或可写。
- 证书文件存在。
- 公网 cloud URL 必须使用 `wss`/`https`；`ws`/`http` 仅允许本机回环开发地址。
- 公网车端 cloud 配置必须声明存在的 `device_cert` 和 `device_key`。
- TURN URL 格式合法。
- `signaling-server --vehicle-config` 会读取同一 `ice` 配置，供
  `/sessions/{session_id}/ice_servers` 发放给当前会话参与者；运行日志和审计只应记录
  server 数量，不记录 TURN credential 明文。
- `credential_mode: password` 使用配置中的长期 TURN credential；`credential_mode:
  turn_rest` 使用 coturn `use-auth-secret`/`static-auth-secret` 生成短期 credential，
  必须配置正数 `credential_ttl_seconds` 和 `static_auth_secret` 或
  `static_auth_secret_file`。
- 控制超时大于命令周期。
- 安全停车制动曲线存在且不是单步全力制动。
- `max_command_gap_ms`、`degraded_timeout_ms`、`control_timeout_ms` 按递增关系配置。
- `control_timeout_ms` 有基于真实车辆制动距离或台架标定的上限依据。
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

本地参考实现提供运行时配置更新门禁：允许日志级别、实时 profile 码率、
上传限速和上传暂停状态通过策略判定，并继续校验运行时值；车辆 ID、
证书路径、车辆适配器、相机列表和控制安全阈值会被判定为需要重启或
禁止热更新。`ComponentLog` 已接入 `logging.level` 运行时更新并按最小级别
过滤组件日志；车端 recorder/uploader 已接入上传限速和上传暂停状态的运行时
应用；实时媒体 runtime controller 已接入实时 profile 码率更新并输出命名
encoder 的 `bitrate` property update，且可绑定到 GStreamer pipeline 命名元素；
同时提供 profile 级弱网降级 hook，可从当前实时 profile 切到预声明的低
fps/低分辨率 profile，pipeline hook 成功后才更新活动状态。
目标媒体主循环仍需在 Ubuntu 工控机端到端验证。
- 安全停车策略。
