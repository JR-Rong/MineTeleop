# 测试与验收

工控机现场联调的无 Docker 部署顺序、动态库放置、车端配置生成和
`target_host_validation_plan.py` 归档流程见
[当前实现与工控机无 Docker 部署](14-current-status-and-ipc-deployment.md)。

## 测试分层

### 单元测试

覆盖：

- 配置解析和校验。
- ControlCommand 范围校验。
- 序号和本地接收间隔判断。
- Safety State Machine 状态转换。
- Upload Queue 状态机。
- 文件命名和元数据生成。

### 集成测试

覆盖：

- 车端 TestPatternSource 到驾驶端显示。
- 驾驶端控制命令到 MockVehicleAdapter。
- WebRTC 信令连接。
- TURN 兜底连接。
- 录像分段保存。
- 上传失败重试。

### 系统测试

覆盖：

- 4 路视频并发。
- 4 路录像并发。
- 控制 20 Hz 长时间运行。
- 5G 网络下连接稳定性。
- 云端断开。
- 驾驶端断开。
- 车端网络断开。
- 磁盘空间不足。

## 验收指标

### 视频

记录：

- 每路 fps。
- 每路码率。
- 端到端延迟。
- 丢帧率。
- 解码失败次数。
- 重连次数。

目标：

- 设计目标 50-150 ms。
- 首版可以分场景记录，不把单一网络结果当作全局承诺。

本地参考实现提供 `VideoAcceptanceMetricsRecorder`，按相机汇总 fps、码率、
端到端延迟、解码帧数、丢帧数、丢帧率、解码失败次数和重连次数，供场景化
验收记录使用。

### 控制

记录：

- 控制发送频率。
- 控制接收频率。
- 控制 RTT。
- 命令乱序数。
- 命令过期数。
- 超时停车触发时间。
- 从最后一条有效控制命令到开始制动的滑行距离。
- 从最后一条有效控制命令到完全停稳的总停车距离。
- 分级制动曲线各阶段的实际生效时间和制动反馈。

目标：

- 正常链路下 20 Hz 稳定。
- 控制断开后在配置阈值内进入安全停车。
- 在最高设计车速、典型载重、坡道/松散路面等边界条件下，基于实测制动距离反推 `control_timeout_ms` 上限，确保总停车距离落在矿区安全范围内。

本地参考实现提供 `ControlAcceptanceMetricsRecorder`，汇总控制发送/接收频率、
RTT、命令乱序/过期计数、超时触发时间、开始制动前滑行时间和滑行距离、
分级制动反馈样本，以及从最后有效命令到完全停稳的总停车距离。真实距离样本
仍需来自台架或封闭场地遥测。

### 录像

记录：

- 分段是否完整。
- 元数据是否完整。
- 文件大小。
- 编码 fps。
- 写盘延迟。
- 磁盘占用增长。

本地参考实现提供 `RecordingAcceptanceMetricsRecorder`，按相机记录分段完整性、
sidecar 元数据完整性、文件大小、编码 fps、写盘延迟和磁盘占用增长，供录像
验收归档使用。

### 上传

记录：

- 上传速度。
- 重试次数。
- 失败原因。
- 对实时流码率/fps 的影响。

本地参考实现提供 `UploadAcceptanceMetricsRecorder`，汇总上传次数、成功/失败数、
总上传字节、平均上传速度、重试次数、失败原因，并按相机对比上传前基线和上传中
实时流 fps/码率变化，供上传验收归档使用。

`scripts/acceptance_metrics_report.py` 可读取上述四类 recorder 对应的 JSONL
样本记录，并输出一条 `acceptance_metrics_report` 汇总和按需生成的
`video_acceptance_metrics`、`control_acceptance_metrics`、
`recording_acceptance_metrics`、`upload_acceptance_metrics` 报告。该脚本用于把
直连、TURN UDP 中继和弱网场景的实测样本统一归档；是否达成 50-150 ms
目标仍必须结合目标环境实测结果判断。汇总和每条指标报告都会输出顶层
`passed/failures`；录像分段或 sidecar 元数据不完整、上传样本失败会显式标记
`passed=false`，供目标主机归档校验拒绝。

## 弱网测试

需要模拟：

- 延迟：50 ms、100 ms、200 ms。
- 抖动：20 ms、50 ms。
- 丢包：1%、3%、5%。
- 带宽限制：5 Mbps、10 Mbps、20 Mbps。

Linux 可使用 `tc netem` 在测试环境模拟。真实工控机生产环境使用前必须确认命令不会影响其他网络业务。

示例：

```bash
sudo tc qdisc add dev eth0 root netem delay 100ms 20ms loss 1%
sudo tc qdisc del dev eth0 root
```

## 硬件编码测试

已完成：

- Docker 内 `h264_vaapi` 单路 1280x720 30 fps 测试成功。
- 本地已生成 4 路 720p30、4 路原分辨率录像、4 路实时+4 路录像同时运行的 VAAPI Docker 压力验证命令计划。
- 本地已生成 GStreamer 硬编和 CPU fallback 插件探测命令。

目标硬件待完成：

- 先运行 `python3 scripts/target_host_validation_plan.py --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so --format shell`
  生成目标主机验证脚本，并把每项命令输出、返回码和自动生成的
  `target_host_validation_archive.jsonl` 归档到验收附件。
- 验收附件中的 `target_host_validation_summary` 必须保留 `command_names`、`command_requirements`、
  `acceptance_scenario`、vehicle config、CAN interface、ChassisControl/MinePilot checkout 路径，以及
  ChassisControl `UI_Test`、MinePilot `merge_ui_test` 分支信息、
  `bridge_build_dir`、`uploader_work_dir`、`minepilot_can_probe_build_dir` 和正整数
  `can_probe_timeout_seconds`；带 artifact 归档执行时还会从
  `chassis.bridge.check` stdout 提取 `chassis_control_commit`、
  `minepilot_commit`、`chassis_control_dirty`、`minepilot_dirty`、
  `bridge_library_path`、`chassis_control_library_path` 以及对应
  `changed_paths` 数量。归档报告会校验 summary 命令清单、完整命令 required/optional
  需求表、bridge stdout `check_count`、bridge 构建目录、CAN probe 绑定字段、commit/dirty revision summary
  和实际 result 记录一致；
  revision summary 缺失或 revision summary 无效时也必须判定验收失败。
- 在目标 CAN 主机上运行
  `python3 scripts/chassis_bridge_check.py --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so`，
  确认 ChassisControl/MinePilot 动态库、`chassis_control.symbols` 导出的
  `Initialize`、`UpdateVehicleState`、`SendCanMessage`、`EmergencyStopWheels()` 等必需符号、`can_db.h`、`can_receiver.h`、
  `can_sender.h`、`src/can_db.cpp`、`src/can_receiver.cpp`、
  `src/can_sender.cpp`、`cmake.configure` 和 `cmake.build` 检查全部 ready；
  归档报告会要求两项 stdout `path` 都匹配 summary `bridge_build_dir`。
- 在同一配置上运行 `python3 vehicle-agent/vehicle_agent.py --config
  /etc/mine-teleop/vehicle-agent.yaml --adapter-status`，归档
  `vehicle_adapter_status` JSON，确认配置的 VehicleAdapter 能打开并报告
  `opened=true`、`healthy=true`，且 `status.library_path` 与 summary
  `bridge_library_path` 一致；summary `chassis_control_library_path` 记录
  bridge 链接的底层 ChassisControl 动态库。
- 目标主机验证计划中的 `vehicle.preflight` 必须归档 `vehicle_preflight`
  JSON，且 `ready=true`、`check_count>0`；否则即使命令返回 0，归档报告也会失败。
- 目标主机验证计划中的 `media.hardware.probes` 必须归档 GStreamer probe 命令、
  3 个硬件压测场景和完整 metrics 字段清单；缺少任一场景、混入额外场景或缺少
  metrics 字段时，即使命令返回 0，归档报告也会失败。
- 目标主机验证计划中的 `network.weak.matrix` 若成功返回，stdout 必须保留
  dry-run 警告、54 个 documented weak-network profile，以及每个 profile 的
  apply/clear `tc netem` 命令；缺少 profile、混入额外 profile 或命令数量不匹配时，
  归档报告会把它标记为证据缺失。
- 在同一配置上运行目标主机验证计划中的 `vehicle.adapter.feedback_poll`
  命令，归档同一 stdout 中的 `vehicle_adapter_status` 和
  `vehicle_adapter_feedback_poll` JSON，确认真实 VehicleAdapter 已在目标 CAN
  interface 上打开，且 C shim 能通过 MinePilot `can_receiver`/`can_db` 拉取
  decoded CAN feedback；该命令使用 `--poll-feedback --require-feedback`，没有成功
  收到反馈帧时必须返回非 0。`python3 scripts/target_host_validation_report.py
  --verify-artifacts` 会解析该命令 stdout 中的 JSONL；即使命令返回 0，只要没有
  real adapter ready 状态或 `vehicle_adapter_feedback_poll.received=true` 证据，
  归档报告也必须失败。
- 在目标 CAN 主机上执行目标主机验证计划中的 `minepilot.can.socket.probe`、
  `minepilot.can.sender.build` 和 `minepilot.can.sender.smoke`，确认 MinePilot
  `can_receiver`/`can_sender` 源码可构建，SocketCAN raw socket 可绑定，并能
  在目标 interface 上限时发送 CAN 帧。归档报告会校验 `minepilot.can.sources`
  stdout `root` 与 summary `minepilot_root` 一致，socket probe stdout
  `script=<minepilot_root>/script/check_can.sh`，sender build stdout
  `target=can_sender_main` 且 `build_dir` 与 summary `minepilot_can_probe_build_dir`
  一致，以及 socket probe 和 sender smoke 的 stdout `interface` 与 summary
  `can_interface` 一致。sender smoke stdout 还必须保留
  `executable=<minepilot_can_probe_build_dir>/can_sender_main` 和 `timeout_seconds`，
  并与 summary 中的 `minepilot_can_probe_build_dir`、`can_probe_timeout_seconds`
  一致；同时必须保留 `startup_banner_seen=true`，避免仅凭手写 JSON 或退出码
  误判发送探针已经启动。
- 在同一配置上执行目标主机验证计划中的 `vehicle.uploader.process_once`，
  使用 `/etc/mine-teleop/vehicle-agent.yaml` 和 `/var/lib/mine-teleop/uploader`
  运行 `vehicle-uploader --service-mode --process-once --json`，归档
  `vehicle_uploader_process_once` JSON；若 stdout 仍是人工文本、缺少 action、
  `passed` 非 true 或 action 为 `failed`，归档报告必须失败。真实上传成功率、
  速度、重试和实时流影响仍由 `upload_acceptance_metrics` 实测报告判定。
- 在工控机上实际执行 4 路 720p30 并发。
- 在工控机上实际执行 4 路原分辨率录像并发。
- 在工控机上实际执行 4 路实时 + 4 路录像同时运行。
- 在工控机媒体环境中实际执行 GStreamer 硬编插件验证。
- 把目标机每路 `ffprobe` 输出和系统指标交给
  `vehicle-media-agent --mode hardware-report`，归档 JSONL 验收记录；任一路
  codec、分辨率、fps 或码率不达标必须返回非 0。
- 把直连、TURN UDP 中继、弱网和场地制动样本保存为 JSONL，并执行目标主机
  验证计划中的 `acceptance.metrics.report`，归档视频、控制、录像和上传四类
  验收指标；缺少任一报告、scenario 不匹配或报告显式失败时，即使命令返回 0，
  归档报告也必须失败。

## 安全测试

必须验证：

- 未授权驾驶端不能建立控制会话。
- 旧 session_id 命令被拒绝。
- seq 倒退命令被拒绝。
- 过期命令被拒绝。
- 驾驶端进程崩溃后安全停车。
- 云端信令断开后安全停车或保持本地安全状态。
- 急停触发后不能自动恢复。
- `max_command_gap_ms` 只触发单次命令过旧处理或链路异常记录，不直接进入急刹。
- `degraded_timeout_ms` 持续超限后进入降级控制。
- `control_timeout_ms` 持续超限后进入 `TIMEOUT_BRAKE`，且该阈值不超过制动距离测试反推的安全上限。

## 场地测试阶段

建议顺序：

1. 桌面仿真。
2. 工控机接真实相机，Mock 控制。
3. 台架接真实控制器，车轮不落地。
4. 封闭场地低速直线。
5. 封闭场地转向和制动。
6. 4 路视频和录像全开。
7. 弱网和断连测试。

任何真实车辆测试都必须有现场物理急停和安全员。
