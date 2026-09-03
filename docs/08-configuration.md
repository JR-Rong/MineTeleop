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

`runtime.media_frame_timeout_ms` 是单路采集等待下一帧的超时，默认 `3000 ms`，
必须为正数。对关键相机而言，这也是“持续无帧”被确认成采集故障前的默认安全确认
窗口；它不是浏览器显示的端到端时延预算，后者由
`hardware.encoding.max_end_to_end_latency_ms` 单独约束。

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
    deceleration_profile:
      - after_ms: 0
        brake: 0.3
      - after_ms: 500
        brake: 0.6
      - after_ms: 1500
        brake: vehicle_defined_max_safe
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
    critical_for_control: true
    reopen_attempts: 3
    reopen_backoff_ms: 500
    backend: auto
    device: /dev/video0
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p
    record_profile: record_source_h265
  - id: rear
    enabled: true
    critical_for_control: true
    reopen_attempts: 3
    reopen_backoff_ms: 500
    backend: auto
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
    tx_queue_length: 100
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
  max_throttle: 0.10
  full_scale_motor_torque_nm: 300.0
  motor_torque_rise_rate_nm_per_s: 0.0
  speed_feedback_timeout_ms: 200
  speed_pid_kp: 1.0
  speed_pid_ki: 0.2
  speed_pid_kd: 0.0
  speed_pid_derivative_filter_tau_ms: 100.0
  speed_pid_max_dt_ms: 100
  hard_overspeed_margin_kph: 3.6
  max_brake_pressure_bar: 100.0
  max_steering_angle_deg: 5.0
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

`control.rate_hz` 当前固定为上游命令 `20 Hz`（50 ms），其他值在 loader 中直接拒绝；
它与 bridge 固定 `20 ms/50 Hz` 的 SocketCAN 发送/PID 周期不是同一个频率。
`max_command_gap_ms`、`degraded_timeout_ms`、`control_timeout_ms` 都必须为正且不大于
60000，并满足 `degraded_timeout_ms < control_timeout_ms`。
`deceleration_profile.after_ms` 从进入 `TIMEOUT_BRAKE` 起算，继续要求非负、按声明顺序
严格递增、制动不下降且最终为 1.0；loader 不会隐式重排。

车端配置决定发布几路视频：`cameras` 中每个 `enabled: true` 的相机创建一条
WebRTC 视频轨，`enabled: false` 的条目既不进入设备 preflight，也不进入媒体
pipeline。控制端按车端 offer 中实际声明的轨道逐路渲染，云服务器只转发鉴权和
信令，不决定路数。当前没有“控制端临时勾选部分轨道”的运行时协商；增减路数需要
修改车端配置并重启车端媒体会话。

`cameras[].enabled` 和 `cameras[].critical_for_control` 必须写成 YAML/TOML boolean
`true`/`false`，不能用带引号字符串。`critical_for_control` 默认 `true`，所以已有
配置在升级后继续采用保守的安全语义；只有确实允许缺失且不影响驾驶视野的辅助相机
才应显式设置为 `false`。当 `runtime.control_enabled=true` 时，至少要有一路
`enabled: true` 且 `critical_for_control: true` 的相机，否则配置加载失败。

`reopen_attempts` 表示同一次媒体运行尝试内、单路采集源允许重开的次数，默认 `3`，
范围 `0..10`；设为 `0` 时首个采集故障直接禁用该 lane。计数不会因中途成功取帧
而无限重置，因此不会形成无界重开循环。`reopen_backoff_ms` 是每次重开前的退避，
默认 `500 ms`，范围 `0..60000 ms`。关键相机的首个已确认故障会立即锁止控制并
触发本地安全停车，同时只重开故障 lane。相机恢复只恢复视频；控制锁存跨同一云端
session 内的 `VehicleMediaRuntime` 重建保持不变。必须结束当前 session，在新 session
中建立新的控制 DataChannel 并重新完成 VCU 握手后才能恢复驾驶权限。V4L2 设备路径
在 USB 拔插或 udev 重建期间
暂时不存在时，仍按 `reopen_attempts` 做有限重试；永久路径错误应由启动 preflight
报告。非关键相机在重开额度耗尽后只禁用自身 lane，其他视频和当前控制不受影响。

`cameras[].backend` 支持 `auto`（默认值）和 `ccg2`。不写该字段等价于 `auto`，继续按
现有规则识别 `testsrc`、普通 V4L2/MJPEG、Aravis/Basler 和 MVS selector，因此旧配置
无需修改。CCG2-8M 必须显式选择 `ccg2`；仓库中的
`configs/vehicle-agent.ccg2-8m.yaml` 给出两路 `/dev/ccg2-channel-0`、
`/dev/ccg2-channel-1` 的
`1920x1080@30` 联调配置。

```yaml
cameras:
  - id: ccg2_channel_0
    backend: ccg2
    device: /dev/ccg2-channel-0
    capture_width: 1920
    capture_height: 1080
    capture_fps: 30
    realtime_profile: realtime_720p30
```

`ccg2` 是对该板卡驱动格式约定的显式兼容边界：V4L2 ioctl 请求并校验驱动报告的
YUYV，但实际 buffer 按 UYVY 字节顺序处理。`capture_width`/`capture_height` 使用
V4L2 协商后应用可见的 `1920x1080`；板卡工具显示的 `1920x1536` input status 只用于
链路诊断，不能填成应用采集高度。配置不会安装内核驱动或初始化板卡；这些步骤仍由
目标机部署流程在启动 MineTeleop 前完成。
ccg2-support 会按 `xdma0_video` 的 sysfs channel index 建立
`/dev/ccg2-channel-0` 至 `/dev/ccg2-channel-7`；生产配置必须引用这些稳定链接，
不要引用枚举顺序不稳定的 `/dev/videoN`。

对 CCG2 而言，`capture_fps` 是 V4L2 输入帧率，realtime profile 的 `fps` 是编码输出
帧率。两者不同时，仅 CCG2 raw pipeline 使用 `videorate` 做显式帧率适配；旧的
MJPEG/testsrc/vendor pipeline 保持原状。`VIDIOC_S_PARM` 成功返回后，其
`timeperframe` 分子、分母必须非零，且必须精确等于配置的 `1/capture_fps`；驱动返回
其它速率时启动失败并报告 `camera_ccg2_fps_mismatch`，不会按近似值继续运行。

示例的 `hardware.encoding` 有意使用 `preferred_encoder: vaapi`、
`fallback_encoder: nvenc`。Ubuntu 22.04/GStreamer 1.20.3 的目标机上，RTX 2000 Ada
NVCodec 会在运行期报告 `Selected preset not supported`，而 Intel VAAPI 已用同一
CCG2 raw 帧验证成功。GStreamer 1.20 的 NVENC pipeline 不写 `preset`/`tune`；1.22
才支持 `preset=p1`，1.24 才同时支持 `tune=ultra-low-latency`。只有升级到 1.24+
并重新完成硬编码验收后，才应在该目标机上改成 NVENC 优先。

实时路径的每路 GStreamer `appsrc` 固定为最多缓存 2 帧，满时丢弃旧帧；其下游
实时 queue 同样为 2 帧有界丢旧。该策略优先保证画面新鲜度，不能用扩大队列来掩盖
编码吞吐不足；应结合 `appsrc_queued_buffers`、编码 FPS 和 capture-to-encode 时延
定位性能瓶颈。启用录像时，编码 tee 的录像 queue 也显式限制为约 2 秒并丢弃旧帧，
避免慢磁盘或 `splitmuxsink` 反压实时 WebRTC 分支。

`hardware.can.interface` 是车端 adapter、MinePilot CAN smoke 和目标主机验收计划共同使用的
SocketCAN 接口名；真实 adapter 配置中
`vehicle_adapter.integration.chassis_control.can_interface` 必须与它一致。
`hardware.can.bitrate` 是运行时必须核对的仲裁波特率，`hardware.can.tx_queue_length`
是运行时设置并复核的 Linux 发送队列长度。应用不会为修改波特率而自动把正在工作的
CAN netdev 置为 DOWN：部署阶段仍需先用 `ip link` 按配置设置波特率并置为 UP；实际波特率
不匹配或接口未 UP 时，真实 adapter 会拒绝启动并报告明确错误。JYR010 bridge 每周期突发
16 个扩展帧，因此真实 adapter 的 `tx_queue_length` 不得小于 16，现场推荐 100。若实际
队列长度不同，运行时设置该值需要 root 或 `CAP_NET_ADMIN`。

`hardware.encoding` 暴露硬件编码与实时验收变量。`preferred_encoder=nvenc`、
`fallback_encoder=vaapi` 定义后端顺序；`preferred_codec=h265`、
`fallback_codec=h264` 定义浏览器支持范围内的 codec 顺序。`media-probe` 直接检查随包
GStreamer factory，`max_end_to_end_latency_ms` 与 `min_realtime_fps` 用于车端和浏览器
验收汇总。DRI 节点变化只修改本节配置，不依赖宿主机 FFmpeg。

`hardware.network.interface` 会进入弱网矩阵和目标主机验收脚本。`field_safety` 用来记录现场
安全链路的最低门禁：调试阶段、目标车速、牵引转矩满量程、是否必须先收到 CAN feedback、
是否必须本地确认急停复位、是否强制时间同步。这些软件门禁不替代现场安全员和物理急停，
但会进入有效配置日志和验收记录。
当前复位实现始终需要本地确认，因此 `require_local_estop_reset` 只能为 `true`；写成
`false` 会 fail closed，不能用未实现的配置制造远程可复位假象。

`field_safety.full_scale_motor_torque_nm` 是车端车速 PID 对每个电机通道可请求的
对称最大转矩。DBC 的八路转矩请求分辨率为 `0.1 Nm`、范围为
`[-800, 838.3] Nm`；普通驾驶会话代码上限取正反方向共同可用幅值 `800 Nm` 的 80%，
即 `640.0 Nm/路`。车端默认值是 `300 Nm/路`，`0` 明确禁用驱动力。
`max_throttle` 不按比例缩小这个转矩上限；例如
`max_throttle=0.10` 表示目标车速最多为 `max_speed_kph` 的 10%，PID 在追踪该目标时
将 `[0,1]` 输出直接乘以会话单电机转矩上限，并可请求最多
`full_scale_motor_torque_nm`。该路径不再使用理想 `m*a` 车辆模型
换算牵引转矩。只能在隔离
台架上逐级调大该转矩上限，并以 CAN 请求和电机反馈共同验收；配置值不是
实测轮端转矩。普通制动和安全停车不由该转矩上限缩放。

`field_safety.motor_torque_rise_rate_nm_per_s` 是车端每个电机的可选加扭斜率，
允许 `0..32000 Nm/s`。`0` 明确关闭额外斜率，PID 输出直接换算为单电机转矩；
正值只限制增加方向，所有减扭、松油、制动、急停和故障清零仍立即生效。启用正值时，
bridge 先计算本周期可达转矩并把它换算为 PID 的动态输出上限，条件积分因而能感知
执行器限制，不是在 PID 之后再盲目截断。该值是车型/执行器标定，同时作为会话 V3
起会话控制参数 `motor_torque_rise_rate_nm_per_s` 的车端默认值与可调范围依据：控制端
面板可在 `[0, 32000] Nm/s` 物理包络内按当前会话修改，修改与 PID 增益变更共享
驻车门槛（N 挡、零速、EPB 驻车、standby/disarmed），会话参数清除后恢复此 YAML
默认值。非 mock 车端必须显式填写；未完成隔离台架标定时应填 `0`，
不能把未经验证的固定斜率当作通用安全值。配置很小的正斜率时，受 DBC `0.1 Nm`
分辨率影响，CAN 请求会表现为若干周期不变后再跳变 `0.1 Nm`，而不是每周期都有变化。

`field_safety.max_brake_pressure_bar` 是八路 EHB 普通驾驶压力的车端硬上限，单位
为 `bar/路`。DBC 每路为 12-bit、`0.1 bar` 分辨率、范围 `0..409.5 bar`；普通驾驶
代码上限取 80%，即 `327.6 bar/路`，车端默认值为 `100 bar/路`。旧的归一化
`field_safety.max_brake` 会被明确拒绝，必须迁移到物理压力字段。控制心跳超时按
`timeout_action.deceleration_profile` 分段执行：自 V3 起的直接压力模式中，小于 1.0 的
`brake` 按 `max_brake_pressure_bar` 换算，例如默认 100 bar 上限下 0.3/0.6 分别为
30/60 bar；最终 `vehicle_defined_max_safe` 对应 1.0，并切到 409.5 bar 安全停车。
急停、物理急停、故障、断开停车以及 bridge 完全收不到上游 apply 时的本地 watchdog
也直接使用 DBC 全量 `409.5 bar/路`，不受普通驾驶上限削弱。所有压力都是 CAN 请求，
不是实测管路压力或制动力。

`max_speed_kph` 是本地车速 PID 的硬车速上限。D/R 且制动为 0 时，目标车速为
`clamp(throttle, 0, max_throttle) × max_speed_kph`；松开纵向输入、刹车或切到 N 挡时目标归零。
车端始终把 `ADU_Tx_VehSpdReq` 按无效的 `0/Q=0` 发送，不让 VCU 同时追踪另一个
车速闭环目标。所有 adapter 的配置范围都是 `0..72 km/h`，`0` 表示明确禁用牵引。
车速闭环、转矩上限和超速熔断必须由隔离台架与实车
分阶段验收；软件单测不等于闭环已验收。

车端 YAML 的五个 `speed_pid_*` 字段与 `motor_torque_rise_rate_nm_per_s` 是启动默认值，
也是会话清除后的恢复值；
`profile_version=3` 允许控制端在满足 N 挡、零速、EPB 驻车且 bridge 为
`standby/disarmed` 时提交一整套 PID 与升扭斜率快照，无需重启车端。更新会撤销旧牵引并复位
PID，成功 ACK 的 `applied_revision` 必须与请求 `seq` 相同。反馈超时、硬超速 margin、
命令超时、降速曲线、时间同步和本地急停复位要求仍只能由车端 YAML 配置，并通过
`control_limits.read_only_control_safety` 只读上报。

启用上述配置时必须同步升级车端 runtime 和 ChassisControl bridge：当前 runtime
要求 ABI version 4、完全一致的 V4 配置结构大小、兼容 V3/V2 大小查询以及
`mine_teleop_chassis_open_v4`，并强制要求 runtime-control V1 配置结构大小查询、
`mine_teleop_chassis_configure_runtime_control_v1` 和
`mine_teleop_chassis_clear_runtime_control_v1`。V4 在 V3 的普通制动压力上限后新增
不可变的单电机加扭斜率；旧 bridge 不会静默忽略该语义，而会在任何 CAN 初始化前因 ABI
不匹配而启动失败。新 bridge 仍为直接 ABI 调用方和兼容性测试保留
`mine_teleop_chassis_open_v1`、`mine_teleop_chassis_open_v2` 与
`mine_teleop_chassis_open_v3`；V1 禁用正牵引并采用默认 `800 ms` 超时，V2 保留负值
表示减速度的旧语义，V3 没有斜率字段，因此直接 PID 转矩不增加额外升扭斜率。旧 vehicle-agent 会被全局 ABI
version 4 门禁明确拒绝，不能依靠这些入口加载新 bridge；runtime 与 bridge 必须原子
成套升级。
进入 Ready 后，若连续
`control.control_timeout_ms` 没有成功 apply，bridge 会撤销车速请求、将转矩置零并施加
标定的安全制动；下一条有效 apply 才会清除该 watchdog 锁存。

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

`vehicle_adapter.type=mock` 可直接无外部依赖运行。配置为 `can` 或
`dynamic_library` 时，必须显式填写 `field_safety.max_speed_kph`、
`field_safety.max_throttle`、`field_safety.full_scale_motor_torque_nm`、
`field_safety.motor_torque_rise_rate_nm_per_s`、
`field_safety.speed_feedback_timeout_ms`、`field_safety.speed_pid_kp`、
`field_safety.speed_pid_ki`、`field_safety.speed_pid_kd`、
`field_safety.speed_pid_derivative_filter_tau_ms`、`field_safety.speed_pid_max_dt_ms`、
`field_safety.hard_overspeed_margin_kph`、`field_safety.max_brake_pressure_bar` 和
`field_safety.max_steering_angle_deg`，并先声明真实车辆接口契约。上述 PID 与超速值只能使用
隔离台架标定结果，不得把示例默认值当作实车验收值。接口契约例如：

```yaml
vehicle_adapter:
  type: dynamic_library
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized_on_wire
    brake_semantics: session_scaled_ehb_pressure_bar
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
      # 当前 ChassisControl 头文件暴露 C++ API，需要稳定的 C shim ABI。
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
      can_interface: can0
      bridge_library_path: /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so
```

以 `configs/vehicle-agent.three-machine.field.yaml` 为现场模板，在部署流程中复制为
`config/vehicle-agent.yaml`，再填入 bridge、CAN、cloud、证书和录像路径。仓库不再
保留旧 Python 配置生成器；修改后必须使用原生入口校验：

```bash
/opt/mine-teleop/bin/mine-teleop-run config-check \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --chassis-bridge-library /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so
/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --preflight
/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --adapter-status
```

仓库提供 `deployments/chassis-control-bridge/` 模板和
`mine_teleop_chassis_bridge.h` 稳定 ABI 头，导出原生 adapter 所需的
`mine_teleop_chassis_open`、`mine_teleop_chassis_open_v1`、
`mine_teleop_chassis_open_v2`、`mine_teleop_chassis_open_v3`、
`mine_teleop_chassis_open_v4`、
`mine_teleop_chassis_apply_state`、
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

`control_timeout_ms` 不能只按网络体验调大。配置前必须结合车速上限、独立安全制动动作、坡道/松散路面和矿区安全距离，反推允许的最大控制超时。本地参考实现会在非 `mock` 车辆适配器配置中要求 `timeout_calibration` 标定证据，并拒绝超过标定上限的 `control_timeout_ms`。

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
  limits:
    initial_target_speed_kph: 2.0
    initial_max_motor_torque_nm: 300.0
    initial_max_brake_pressure_bar: 100.0
    initial_service_brake_pressure_bar: 30.0
    initial_hard_brake_pressure_bar: 100.0
    initial_max_steering_angle_deg: 3.0
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
`control.limits` 提供目标车速、转矩、压力和最大转角等驾驶参数；页面再合并车端
`control_limits` 上报的五个 `default_speed_pid_*` 默认值与
`default_motor_torque_rise_rate_nm_per_s`，组成提交给车端确认的
`profile_version=3` 初始完整快照。示例默认驾驶参数是目标车速 `2 km/h`、单电机最大转矩
`300 Nm`、普通制动最大/缓刹/急刹压力 `100/30/100 bar/路` 和最大转角。它们分别受车端 `max_speed_kph * max_throttle`、
`full_scale_motor_torque_nm` 和 `max_brake_pressure_bar` 硬上限约束；控制端设置不能
提高车端上限。PID 可在安全驻车门禁内按会话热更新；反馈超时、超速 margin 和其他
`read_only_control_safety` 字段只能在车端配置，驾驶端不能覆盖。
`control.keyboard` 已删除且不再是可配置接口。键位固定为
`ArrowLeft`/`A` 左转、`ArrowRight`/`D` 右转、`ArrowUp`/`W` 前进、
`ArrowDown`/`S` 倒车、`Space` 缓刹、`B` 急刹、`E` 急停。加载器如果
发现仓库外旧配置仍包含 `control.keyboard`，会直接拒绝启动，避免运维误以为
某个未生效的键位绑定已被应用。
`control.gamepad` 的轴编号来自浏览器 Gamepad API；标准映射手柄使用浏览器规定的
左摇杆 X、右/左扳机，非标准方向盘/踏板使用这里的轴配置。`*_center`/`*_rest`
和 `*_range` 可写入现场测量值，也可以在浏览器中做本次运行有效的中心与量程校准。
如果轴顺序或方向不同，只需要调整配置，不需要改控制核心。
三个 `*_brake_pressure_bar` 都是物理 EHB 压力请求，必须满足
`0 <= service <= hard <= max <= vehicle max_brake_pressure_bar`，同时不得超过普通
驾驶代码上限 `327.6 bar/路`。驾驶端的 `Space` 缓刹和 `B` 急刹仍映射到同一个 v1
`brake` 线协议标量；同时按下时急刹优先，车端按已确认会话最大压力还原成 bar。
本 PR 中缓刹为直接 `service_brake_pressure_bar`，不运行制动 PID，也不做压力 ramp；
急刹直接请求 `hard_brake_pressure_bar`。任何制动会把油门与目标车速置零、复位车速
PID、清零八路电机扭矩，同时保留转向。页面修改参数会先清空输入并要求车端确认，
标准 `/api/control` 只准备 0..1 的模拟制动标量；物理压力始终由车端当前已确认的
会话 profile 还原。
默认 profile 只在驻车准入已满足（或车端明确报告 mock/无需握手且 adapter ready）后，
每个控制链路 generation 自动提交一次；若车端拒绝，后续状态消息不会自动换新序号重试，
必须由驾驶员显式确认后再次提交。
对真实 VCU adapter，首次 profile、提高目标车速/转矩，以及修改任一转向、PID 或制动
压力字段（即使降低）都必须先满足 N 挡、有效零速、电子驻车已拉起，并处于
`standby/disarmed`；浏览器预检、core 与 bridge 门禁使用同一口径。
旧 `GET /api/control-limits` 只保留归一化比例的只读兼容；旧 `POST /api/control-limits`
固定返回 `410 Gone`，调用方必须迁移到带会话鉴权与车端 ACK 的 `/api/control-profile`。
急停、物理急停、故障、断开停车和 bridge 本地 apply watchdog 走独立
`409.5 bar/路` 安全路径，不受这些普通驾驶压力上限削弱。控制心跳超时先按
`timeout_action.deceleration_profile` 的普通压力分段执行，最终 1.0 阶段才切到
409.5 bar。
旧的 `POST /api/control/keyboard` 与 `POST /api/control/gamepad` 固定返回 `410 Gone`。
这两个接口无法携带或验证车端 profile ACK；若在新 profile 被拒绝时继续按本地预设生成
制动标量，车辆会按旧 active profile 还原成不同的 bar 值。旧调用方必须迁移到
`/api/control-profile` 等待精确 ACK，再用标准 `/api/control` 发送明确的 v1 控制命令。

### 升级迁移

- 升级前从所有仓库外驾驶端 YAML 删除整个 `control.keyboard` 段；键位不会从旧值自动迁移。
- 删除旧的 `initial_max_throttle`、`initial_service_brake`、`initial_hard_brake` 和
  车端 `field_safety.max_brake`；加载器会拒绝这些归一化旧字段。按上述物理单位显式
  配置目标速度、单电机转矩以及三项 EHB bar 值。
- 检查所有仓库外车端的 `field_safety.max_speed_kph`。加载器通用范围已收紧为
  `[0, 72] km/h`，`0` 明确禁用牵引；大于 `72` 的旧值会启动失败，不会静默截断。
  必须根据本地 PID 车速上限和隔离台架结果显式选择新值。
- 不得把默认 `300 Nm/路` 或普通最大 `100 bar/路` 当作已标定实车值。旧语义下有效
  转矩上限可能约为 `full_scale_motor_torque_nm × max_throttle`；升级时应从旧有效上限
  或更小值开始，经隔离台架逐级标定。非 mock 配置缺少上述任一必填车速、比例、转矩、PID/反馈、
  超速、制动或转向门禁时继续 fail closed，不会补默认值启动真实 adapter。
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
- 已单独确认控制超时 0.3/0.6 分段的物理压力、最终 1.0 阶段以及故障/断链/急停的
  八路 `409.5 bar` 安全制动语义和 VCU 硬件响应；409.5 bar 路径不受普通会话压力
  上限影响。
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
控制安全 YAML 本身仍不热重载；唯一例外是已鉴权会话通过
`session_control_profile` V2 原子设置受限的目标车速、转矩、普通压力、转角和 PID，
断链/故障/会话替换时立即清除并恢复车端默认 PID。
目标媒体主循环仍需在 Ubuntu 工控机端到端验证。
- 安全停车策略。
