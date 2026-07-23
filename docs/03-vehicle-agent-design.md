# 车端 Agent 设计

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

## 职责

Vehicle Agent 运行在车端 Ubuntu 工控机，负责：

- 读取配置。
- 管理相机采集。
- 实时编码和 WebRTC 推流。
- 原分辨率录像编码和分段保存。
- 接收驾驶端控制命令。
- 执行安全状态机。
- 调用车辆控制适配器。
- 采集基础车辆状态并回传。
- 管理上传队列。
- 输出运行日志和健康状态。

安全关键职责必须与媒体 pipeline 做故障隔离。首版推荐独立 `vehicle-control-agent` 进程承载 Control Receiver、Safety State Machine 和 Vehicle Adapter；如果联调阶段暂时使用同一可执行文件，也必须使用独立高优先级线程、看门狗和有界队列，确保媒体编码或上传卡死不会阻塞安全停车。

## 模块划分

### Config Manager

负责加载和校验配置。

能力：

- 从 YAML/TOML 读取配置。
- 校验相机 ID 唯一。
- 校验码率、帧率、路径、超时阈值。
- 输出最终生效配置到日志。
- 支持后续热更新部分非危险配置。

原生 C++ 运行时会先输出脱敏后的有效配置摘要，设备 token、证书和 TURN
凭据不会写入日志。`vehicle-agent --preflight` 检查相机、CAN interface 和
bridge 动态库；`vehicle-agent --adapter-status` 会实际打开配置的
VehicleAdapter、输出 `vehicle_adapter_status`，失败时返回非 0。

危险配置如车辆控制适配器、设备证书路径、车辆 ID，不建议运行时热更新。

### Camera Manager

负责管理多个 Camera Source。

接口概念：

```text
CameraSource
  start()
  stop()
  readFrame()
  getStatus()
```

首版实现：

- `V4L2CameraSource`：接 USB 或 V4L2 兼容设备。
- `TestPatternSource`：用于无相机开发联调。
- `FileReplaySource`：用于回放测试。

本地参考实现支持 `V4L2CameraSource` 的设备路径校验和 GStreamer source
片段生成；真实 USB/V4L2 相机采集仍需在 Ubuntu 工控机和目标相机上验证。

本地参考实现支持 JSONL 格式的 `FileReplaySource`。每行是一帧，字段为
`seq`、`width`、`height`、`timestamp_ms` 和 `pattern`；回放耗尽时该路
相机进入明确错误，其他相机仍由 supervisor 继续轮询。

后续实现：

- `GMSLCameraSource`
- `CSICameraSource`
- 厂商 SDK Camera Source

### Media Pipeline

每路相机建议独立 pipeline。

逻辑结构：

```text
Camera Source
  -> Capture Queue
  -> Tee
    -> Realtime Branch
      -> Scale/Convert
      -> Low Latency Encoder
      -> WebRTC Sender
    -> Record Branch
      -> Record Encoder
      -> Segment Writer
```

实时分支原则：

- 队列长度短。
- 丢弃旧帧。
- 使用低延迟编码参数。
- 码率可动态调整。

录像分支原则：

- 不影响实时分支。
- 允许稍高缓冲。
- 以文件完整性和画质为优先。

### Control Receiver

负责接收驾驶端控制命令。

处理步骤：

1. 校验 `protocol_version`。
2. 校验会话 ID。
3. 校验控制权。
4. 校验序号是否新于最近命令。
5. 使用车端本地接收时间更新控制心跳，并基于到达间隔判断命令是否过旧。
6. 将驾驶端时间戳保留用于审计和延迟估算，不直接用跨机器时钟差做安全判定。
7. 将命令交给 Safety State Machine。

### Safety State Machine

负责确保车辆控制安全。

状态建议：

- `INIT`：启动中，禁止车辆动作。
- `STANDBY`：待命，未获得有效控制。
- `CONTROL_ACTIVE`：控制链路有效，允许下发命令。
- `TIMEOUT_BRAKE`：控制心跳超时，执行安全停车。
- `ESTOP`：急停锁定，需要人工复位。
- `FAULT`：车辆或系统故障，禁止继续控制。

转换规则：

- `INIT -> STANDBY`：配置加载、通信初始化完成。
- `STANDBY -> CONTROL_ACTIVE`：会话建立且收到有效控制心跳。
- `CONTROL_ACTIVE -> TIMEOUT_BRAKE`：超过 `control_timeout_ms` 未收到有效命令。
- `CONTROL_ACTIVE -> ESTOP`：驾驶端或车端触发急停。
- `TIMEOUT_BRAKE -> STANDBY`：车辆停稳且会话已释放或复位流程完成。
- `ESTOP -> STANDBY`：现场物理确认和授权复位完成。
- 任意状态 -> `FAULT`：检测到不可恢复故障。

急停必须锁存。车端收到一次 `estop=true` 即进入 `ESTOP`，后续是否继续收到驾驶端急停包不影响锁存状态。解除急停不能只依赖驾驶端 UI 按钮。本地参考实现要求复位调用带本地确认和授权人，并写入 `estop_reset` 审计事件；真实车辆接入前必须定义现场物理确认、授权人和复位记录。

### Vehicle Adapter

车辆控制接口尚未确定，可能是 CAN，也可能是动态库封装接口。因此必须通过适配层隔离。

统一接口概念：

```text
VehicleAdapter
  open()
  close()
  applyControl(command)
  applySafeStop()
  pollFeedback()
  readTelemetry()
  getStatus()
```

首版实现：

- `MockVehicleAdapter`：打印/记录控制命令，不接真实车辆。
- 原生运行时只允许 `vehicle_adapter.type=mock` 无外部依赖运行；`can` 和
  `dynamic_library` 必须声明 C shim bridge、CAN interface 和超时标定证据。
  缺少 bridge 时启动失败，不能静默退回 Mock。
- `vehicle-agent` 的无模式默认闭环是开发 Mock demo，只能在 `vehicle_adapter.type=mock`
  配置下运行。真实 adapter 配置必须显式走 `--run-loop` 或 `--adapter-status`；
  否则进程返回非 0 并输出 `vehicle_agent_mode_error`，避免目标机误以为真实底盘链路已经启动。
- `dynamic_library` 通过原生 C++ `DynamicLibraryVehicleAdapter` 加载稳定 C shim
  ABI。仓库内的 `deployments/chassis-control-bridge/` 把 ChassisControl 和
  MinePilot CAN 接口封装为该 ABI。

后续实现：

- `CanVehicleAdapter`：当前可通过 ChassisControl C shim 间接使用 SocketCAN/厂商 CAN 后端；后续若需要绕过 ChassisControl，可再增加直接 SocketCAN 或厂商 CAN 卡 SDK adapter。

`mine_teleop_chassis_bridge.h` 声明控制下发、急停、反馈轮询、telemetry 和关闭
接口。bridge CMake 会校验所需的 ChassisControl/MinePilot 头文件和源码，并把
decoded CAN feedback 交给原生 adapter。构建命令、依赖和启动前检查统一见
`deployments/chassis-control-bridge/README.md`；目标 Ubuntu 车辆主机仍需完成
真实 `.so` 构建、SocketCAN/CAN 卡和底盘联调验证。
车端长期控制服务和 `vehicle-agent --run-loop` 摘要也只读取 adapter `get_status()`
中的 `applied_command_count`，避免真实 C shim adapter 运行路径依赖 Mock 专用的
内部命令列表。

### Telemetry Publisher

负责周期性回传状态。

首版状态：

- 速度。
- 档位。
- 转向反馈。
- 油门反馈。
- 刹车反馈。
- 安全状态机状态。
- 故障标志。
- 控制延迟。
- 视频码率。
- 编码 fps。
- CPU/GPU/内存/磁盘占用。

本地参考实现的 `TelemetryPublisher` 保留每路视频状态中的 `fault` 和
`encoder` 字段，并把系统 `fault_flags` 与每路视频故障聚合到顶层
`fault_flags`，例如 `video.front.hardware_encoder_unavailable`。Mock
Telemetry 仍明确标记为非真实车辆反馈。

### Recorder

负责分段视频文件保存。

建议：

- 每路相机独立目录。
- 文件名包含时间、车辆 ID、相机 ID、会话 ID。
- 元数据写入 sidecar JSON。
- 先写临时文件，完成后原子 rename。
- 磁盘空间低于阈值时停止新增录像或删除最旧已上传文件。
- 按配置计算目标保留时长。若录像产生速率持续高于上传带宽，应优先保护实时控制和已完成文件完整性，并通过降码率、暂停录像、扩容或只删除已上传文件等策略处理，不能默认删除未上传片段而不告警。

### Uploader

负责低优先级上传。

能力：

- 扫描待上传文件。
- 逐文件上传视频片段和 sidecar 元数据。
- 上传成功标记。
- 上传失败退避重试。
- 限速。
- 可暂停。
- 可恢复。
- 使用预签名 URL 时，在每次上传前检查有效期，过期或即将过期时重新向云端申请凭证。

本地 `VehicleRecorderUploader.scan_pending_segments()` 会扫描录像根目录下
`upload_state=pending` 且视频文件仍存在的 sidecar，重新向 Upload API 申请
video/metadata 两类凭证后恢复上传队列；已在队列中的片段不会重复入队。
`process_once()` 在队列无可执行项时会先触发一次扫描，再决定上传或返回
`idle`。通过 `from_config()` 创建的 uploader 会把 `upload.trigger_segments`、
`upload.trigger_bytes_mb` 和 `upload.trigger_interval_seconds` 接入实际调度；
未达到触发条件时保持 `pending` 并返回 `wait`。
当 `upload.enabled=false` 时，recorder 仍写入视频和 sidecar，但不会申请上传
凭证、不会把片段加入上传队列，也不会扫描历史 pending sidecar；`process_once()`
返回 `disabled`。
上传目标写入或对象存储适配器抛出 IO 异常时，uploader 会登记失败并进入
`retry_wait`，而不是把片段留在 `uploading` 状态。

## 车端启动顺序

1. 加载配置。
2. 初始化日志。
3. 检查设备和权限。
4. 初始化车辆适配器。
5. 初始化相机。
6. 初始化媒体 pipeline。
7. 连接云端信令。
8. 进入待命状态。
9. 会话建立后启动实时推流和控制接收。
10. 根据配置启动录像和上传队列。

本地参考实现提供只读 `VehiclePreflightChecker`：启动前检查启用相机设备、
录像目录写权限和指定硬编设备节点，输出每项 `ready`、`missing`、
`not_readable` 或 `not_writable` 状态，不自动修改权限或创建系统设备。
`vehicle-agent --preflight --hardware-device /dev/dri/renderD128` 会以 JSONL
输出汇总和逐项检查；全部检查 ready/skipped 时返回 0，否则返回 2，便于
systemd `ExecStartPre` 或部署脚本阻止带缺失设备的真实车端启动。

## 车端异常处理

### 相机异常

- 单路相机失败不应导致全车端退出。
- 驾驶端 UI 显示对应相机故障。
- 录像和实时流分别记录错误。

### 编码异常

- 优先尝试重启对应相机 pipeline。
- 如果硬编失败，可按配置降级 CPU 编码。
- 降级必须写日志和上报状态。

本地参考实现提供 `MediaFaultRecoveryPolicy`：watchdog 判定单路 pipeline
卡死后生成 `restart_camera_pipeline` 恢复动作、`media_pipeline_restart_requested`
组件日志和该相机的 `reconnecting` 视频状态；硬编失败且存在 CPU fallback
时生成 `fallback_encoder` 动作、`media_encoder_fallback` 组件日志和
`degraded` 视频状态。`MediaFaultRecoveryExecutor` 可把这些决策绑定到媒体主循环
控制器的 `restart_camera_pipeline(camera_id)` 和
`switch_camera_encoder(camera_id, encoder)` 方法；真实 GStreamer pipeline 重启和
编码器切换仍需在目标车端运行时端到端验证。

### 网络异常

- 信令断开：尝试重连。
- 媒体断开：尝试 ICE restart 或重建会话。

本地参考实现提供 `RealtimeConnectionRecoveryPolicy`：信令断开时生成
`reconnect_signaling` 决策、退避延迟和 `signaling_reconnect_requested`
组件日志；媒体断开时先生成 `ice_restart` 决策，超过配置次数后生成
`rebuild_session` 决策，并分别写入 `media_ice_restart_requested` 或
`media_session_rebuild_requested` 组件日志。`RealtimeConnectionRecoveryExecutor`
可把这些决策绑定到实时连接控制器的 `reconnect_signaling(retry_delay_ms)`、
`restart_ice(camera_id)` 和 `rebuild_media_session(camera_id)` 方法；真实
WebRTC reconnect、ICE restart 和 session rebuild 操作仍需目标运行时端到端验证。
- 控制心跳超时：立即安全停车。

### 磁盘异常

- 低水位告警。
- 达到硬阈值后停止录像或删除已上传旧文件。
- 不允许影响控制安全状态机。
