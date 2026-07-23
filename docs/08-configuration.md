# 配置体系

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

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

## 信令服务多身份配置

独立信令进程的多驾驶员/多车辆身份使用单独的 YAML。仓库示例
`configs/signaling-server.2x2.dev.yaml` 使用环境变量引用，不包含 secret 明文：

```yaml
auth:
  drivers:
    - id: driver-console-001
      password_env: MINE_TELEOP_DRIVER_001_PASSWORD
      vehicles: [vehicle-001]
    - id: driver-console-002
      password_env: MINE_TELEOP_DRIVER_002_PASSWORD
      vehicles: [vehicle-002]
  vehicles:
    - id: vehicle-001
      device_token_env: MINE_TELEOP_VEHICLE_001_TOKEN
    - id: vehicle-002
      device_token_env: MINE_TELEOP_VEHICLE_002_TOKEN
```

每个驾驶员必须配置非空车辆白名单；白名单只能引用本文件中声明的车辆。驾驶员
必须且只能二选一配置 `password_file`/`password_env`，车辆同样二选一配置
`device_token_file`/`device_token_env`。相对 secret 文件按 YAML 所在目录解析，现场
文件应设为 `0600`。先执行以下命令做无监听校验，再启动服务：

```bash
mine-teleop-signaling-server \
  --config /etc/mine-teleop/signaling-server.yaml \
  --validate-config
```

多身份配置不能与旧的 `--driver-id`、`--driver-password`、`--vehicle-id`、
`--device-token` 或对应的单身份 secret 环境变量混用；混用会启动失败。

## 车端配置示例

当前安装包携带 `config/vehicle-agent.yaml`。其中 `runtime` 决定统一前台入口启动哪些服务，
`cloud.device_token_file` 只保存令牌文件路径；令牌内容不进入安装包：

```yaml
runtime:
  control_enabled: true
  media_enabled: true
  control_log_commands: true
  teleop_poll_interval_ms: 500
  media_frame_timeout_ms: 3000
  media_capture_interval_ms: 0

cloud:
  signaling_url: wss://teleop-field.internal:6000/signaling
  device_token_file: device-token
  resolve:
    - teleop-field.internal:6000:60.205.213.254
  ca_bundle: mine-teleop-field-root.crt
```

相对的 `device_token_file` 按 YAML 所在目录解析。现场只需创建权限为 `0600` 的
`config/device-token`，随后执行 `bin/mine-teleop-run`。`cloud.resolve` 的每一项使用
libcurl 的 `host:port:address` 格式，只影响当前进程；连接仍以
`teleop-field.internal` 做 SNI 和证书主机名校验，不修改系统 DNS/hosts。
`cloud.ca_bundle` 可使用相对配置文件的路径，必须指向可信 CA 文件。当前三机现场
路径依靠这两个字段直接连接云端，不需要 SSH、SOCKS 或 FRP。500 ms 的空闲会话
轮询既可在无会话时保持车端在线，也避免把云端 API 限流预算消耗在高频空轮询上；
已建立会话后的 WebSocket/DataChannel 不使用该轮询周期。

车辆和控制端的后续 GET/WSS 请求分别用
`X-Mine-Teleop-Device-Token`、`X-Mine-Teleop-Driver-Token` header 携带凭据，
不会把 token 拼入 URL。服务端对 query token 的读取仅用于旧客户端迁移兼容；
登录和车辆在线注册仍为 TLS 下的 POST JSON。

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
      codec: h265
      encoder: auto
      width: 1280
      height: 720
      fps: 30
      bitrate_kbps: 3000
      keyframe_interval_frames: 30
      low_latency: true
    realtime_480p15:
      codec: h264
      encoder: auto
      width: 854
      height: 480
      fps: 15
      bitrate_kbps: 1200
      keyframe_interval_frames: 30
      low_latency: true
  record_profiles:
    record_source_h265:
      codec: h265
      encoder: reuse_realtime
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
    record_profile: record_source_h265
  - id: rear
    enabled: true
    device: /dev/video1
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265

hardware:
  can:
    interface: can0
    bitrate: 500000
    restart_ms: 100
    probe_timeout_seconds: 3
  encoding:
    vaapi_render_device: /dev/dri/renderD128
    dri_card_device: /dev/dri/card1
    preferred_encoder: nvenc
    fallback_encoder: vaapi
    preferred_codec: h265
    fallback_codec: h264
    require_hardware_encoder: true
    max_end_to_end_latency_ms: 200
    min_realtime_fps: 20
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

车端配置决定发布几路视频：`cameras` 中每个 `enabled: true` 的相机创建一条
WebRTC 视频轨，`enabled: false` 的条目既不进入设备 preflight，也不进入媒体
pipeline。控制端按车端 offer 中实际声明的轨道逐路渲染，云服务器只转发鉴权和
信令，不决定路数。当前没有“控制端临时勾选部分轨道”的运行时协商；增减路数需要
修改车端配置并重启车端媒体会话。

`cameras[].enabled` 必须写成 YAML/TOML boolean `true`/`false`，不能用带引号字符串。

`hardware.can.interface` 是车端 adapter、MinePilot CAN smoke 和目标主机验收计划共同使用的
SocketCAN 接口名；真实 adapter 配置中
`vehicle_adapter.integration.chassis_control.can_interface` 必须与它一致。`hardware.can.bitrate`
用于现场 CAN 口配置记录和部署命令，应用进程不会自动改 Linux netdev，需要现场用 `ip link`
按该值配置。

`hardware.encoding` 暴露硬件编码与实时验收变量。`preferred_encoder=nvenc`、
`fallback_encoder=vaapi` 定义后端顺序；`preferred_codec=h265`、
`fallback_codec=h264` 定义浏览器支持范围内的 codec 顺序。`media-probe` 直接检查随包
GStreamer factory，`max_end_to_end_latency_ms` 与 `min_realtime_fps` 用于车端和浏览器
验收汇总。DRI 节点变化只修改本节配置，不依赖宿主机 FFmpeg。

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

logging:
  browser_event_log: ../.local/logs/control-browser-events.jsonl
  browser_event_log_max_bytes: 2097152
  browser_event_log_files: 3

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
    steering_inverted: false
    throttle_inverted: true
    brake_inverted: true
    steering_center: 0.0
    steering_range: 1.0
    throttle_rest: 1.0
    throttle_range: 2.0
    brake_rest: 1.0
    brake_range: 2.0
    estop_button: 0
```

`ui.show_debug_overlay` 必须写成 YAML/TOML boolean `true`/`false`，不能用带引号
字符串，避免调试层在正式驾驶端被误启用或误关闭。
`control.gamepad` 的轴编号来自浏览器 Gamepad API；标准映射手柄使用浏览器规定的
左摇杆 X、右/左扳机，非标准方向盘/踏板使用这里的轴配置。`*_center`/`*_rest`
和 `*_range` 可写入现场测量值，也可以在浏览器中做本次运行有效的中心与量程校准。
如果轴顺序或方向不同，只需要调整配置，不需要改控制核心。
`logging.browser_event_log` 的相对路径以 YAML 文件所在目录为基准；默认值把日志
写入控制端包根目录的 `.local/logs/`。`browser_event_log_files` 包含当前文件，
因此值 `3` 表示当前文件加 `.1`、`.2` 两个备份。凭据类字段会被递归脱敏，但部署
时仍应限制日志目录权限，并按现场保留策略采集或销毁日志。

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
- C++ `signaling-server` 使用 `--stun-urls`、`--turn-urls`、`--turn-realm`、
  `--turn-static-auth-secret-file` 和 `--turn-credential-ttl-seconds` 提供
  `/sessions/{session_id}/ice_servers`；对应环境变量适合容器部署。运行日志和审计
  只记录 server 数量和到期时间，不记录 TURN credential 明文。
- 当前服务端只签发 coturn `use-auth-secret`/`static-auth-secret` 短期 credential，
  不向客户端发放长期 TURN 密码。TURN URL 非空时 realm、secret 和正数 TTL 都是
  必填项。
- `signaling-server` 的 `--login-max-failures`、`--login-failure-window-ms` 和
  `--login-lockout-ms` 分别控制登录失败阈值、计数窗口和锁定时间，三者必须为
  正数；部署示例默认 `5`、`60000`、`300000`。已配置账号各自限流，未知账号
  共用一个有界桶。systemd 部署可用对应的
  `MINE_TELEOP_LOGIN_MAX_FAILURES`、`MINE_TELEOP_LOGIN_FAILURE_WINDOW_MS`、
  `MINE_TELEOP_LOGIN_LOCKOUT_MS`，显式 CLI 参数优先。
- 通用 HTTP/WSS 来源限流由 `--api-rate-limit-requests`、
  `--api-rate-limit-window-ms`、`--api-rate-limit-max-sources` 和
  `--trusted-proxy-addresses` 控制；部署示例默认 `600`、`60000`、`4096`、
  `127.0.0.1,::1`。对应环境变量为
  `MINE_TELEOP_API_RATE_LIMIT_REQUESTS`、`MINE_TELEOP_API_RATE_LIMIT_WINDOW_MS`、
  `MINE_TELEOP_API_RATE_LIMIT_MAX_SOURCES`、
  `MINE_TELEOP_TRUSTED_PROXY_ADDRESSES`，显式 CLI 参数优先。
- 只有 TCP 直接对端与可信代理 IP 精确匹配时才读取 `X-Forwarded-For`；否则始终
  以直接对端 IP 计数。来源表达到上限后，新来源共用一个有界溢出桶，不继续增长
  内存。该计数器为单进程固定窗口；多实例部署仍需共享边缘限流，认证后的高风险
  路由仍需独立配额。
- signaling 审计固定每个 UTC 小时归档一次，默认保留最近 7 天；
  `--audit-log-retention-days`/`MINE_TELEOP_AUDIT_LOG_RETENTION_DAYS` 可设置
  1 到 365 天。`--audit-log-max-bytes` 与 `--audit-log-files` 设置当前小时内的
  单分片大小和分片上限，默认 64 MiB、5 个文件；对应环境变量为
  `MINE_TELEOP_AUDIT_LOG_MAX_BYTES`、`MINE_TELEOP_AUDIT_LOG_FILES`。大小至少
  1024 bytes，分片数范围为 1 到 20。全部轮转、追加、flush 和过期清理由同一
  写入锁保护。每次服务构造会先写入 UTC `signaling_service_started`，审计目录
  不存在或不可写时启动失败。
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
