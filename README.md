# Mine Teleop

矿车遥操系统项目文档。

本目录用于沉淀车端工控机、远端模拟驾驶器、云端信令/中继/上传服务之间的需求、设计、实现计划和验证方案。当前文档来自前期需求讨论和工控机硬件编码排查结果。

## 当前结论

- 场景：封闭矿区矿车，最高车速不超过 40 km/h。
- 部署：车端为 Ubuntu 工控机，驾驶端优先 Windows，也可能 Linux。
- 网络：车端和驾驶端不在同一局域网，云服务器有固定 IP，可以新增支持 UDP 的节点。
- 视频：默认 4 路相机，数量可配置；实时流首版目标 720p 30 fps，可配置分辨率、帧率和码率。
- 控制：驾驶端发送档位、转向、油门、刹车；首版控制输入使用键盘/软件控件。
- 控制频率：首版 20 Hz。
- 安全：控制链路超时后车端执行分级减速安全停车；急停由车端锁存，解除必须走显式复位流程。
- 录像：车端按相机采集原分辨率编码保存，分段后逐文件上传云端，不默认做 zip/tar 打包压缩。
- 编码硬件：当前工控机无 NVIDIA；Intel Alder Lake-S GT1 核显存在，Docker 中已验证 `h264_vaapi` 可编码 1280x720 30 fps。
- 推荐主线：C++/Qt + GStreamer/WebRTC；媒体编码优先 Intel VAAPI/QSV，CPU x264 兜底；云端提供 HTTPS/WebSocket 信令和 STUN/TURN 兜底。

## 文档导航

- [项目背景与决策](docs/00-project-context.md)
- [需求规格](docs/01-requirements.md)
- [系统架构](docs/02-system-architecture.md)
- [车端 Agent 设计](docs/03-vehicle-agent-design.md)
- [驾驶端 Console 设计](docs/04-driver-console-design.md)
- [云端服务设计](docs/05-cloud-services-design.md)
- [视频、录像与上传](docs/06-media-recording-upload.md)
- [控制协议与安全停车](docs/07-control-and-safety.md)
- [配置体系](docs/08-configuration.md)
- [硬件与环境排查结论](docs/09-hardware-and-environment.md)
- [实施计划](docs/10-implementation-plan.md)
- [测试与验收](docs/11-testing-and-validation.md)
- [运维与排障](docs/12-operations-and-troubleshooting.md)
- [待确认问题](docs/13-open-questions.md)
- [当前实现与工控机无 Docker 部署](docs/14-current-status-and-ipc-deployment.md)
- [Ubuntu Bundle 软件说明](docs/15-ubuntu-bundle-software.md)
- [Ubuntu Bundle 使用说明](docs/16-ubuntu-bundle-usage.md)
- [Ubuntu Bundle 架构说明](docs/17-ubuntu-bundle-architecture.md)
- [控制端 Docker 程序与 Smoke](docs/18-control-plane-docker-smoke.md)
- ADR:
  - [0001: 传输与媒体栈选择](docs/adr/0001-transport-and-media-stack.md)
  - [0002: 硬件编码策略](docs/adr/0002-hardware-encoding-strategy.md)

## 推荐第一阶段目标

第一阶段不直接做完整平台，而是做可验证闭环：

1. 车端用 1 路测试源或真实相机采集视频。
2. 车端使用 Intel VAAPI H.264 编码。
3. 通过云端信令建立 WebRTC 连接。
4. 驾驶端显示实时视频。
5. 驾驶端以 20 Hz 发送控制命令。
6. 车端接收控制命令并输出到 Mock Vehicle Adapter。
7. 控制断开或心跳超时时进入分级减速安全停车状态。
8. 车端分段保存视频文件，并模拟逐文件上传。

完成这个闭环后，再扩展到 4 路相机、真实车辆控制接口、录像上传、权限管理和运维监控。

## 当前可运行实现

当前仓库包含一个离线可测试的 Python 参考实现，用于把第一阶段 Mock
闭环中的安全关键语义先固定下来：

- `mine_teleop/`：配置校验、控制协议、安全状态机、媒体测试源、录像分段元数据、上传队列、信令会话和 Mock 闭环。
- `configs/vehicle-agent.dev.yaml`：车端开发配置样例。
- `configs/driver-console.dev.yaml`：驾驶端开发配置样例。
- `vehicle-agent/vehicle_agent.py`：车端 Mock 闭环入口和长期控制服务循环模拟入口；默认
  demo 只允许 `vehicle_adapter.type=mock`，真实 adapter 配置必须显式使用
  `--run-loop` 或 `--adapter-status`，避免静默退回 Mock。
- `vehicle-media-agent/vehicle_media_agent.py`：按全部启用相机生成独立媒体 pipeline，并提供 VAAPI Docker、GStreamer 插件和四路硬件编码验证命令生成入口。
- `vehicle-uploader/vehicle_uploader.py`：录像分段、本地上传队列和 Upload API 闭环 demo。
- `driver-console/driver_console.py`：读取驾驶端配置，既可运行 20 Hz 控制命令示例，也可用
  `--serve` 启动 Docker 友好的 HTTP 控制端程序；该程序提供驾驶端 UI/API、会话连接、
  WebRTC offer/answer 信令转发、浏览器视频/DataChannel wiring 和控制命令转发入口。
- `mine_teleop/control_console_container.py`：控制端容器默认入口，读取
  `MINE_TELEOP_DRIVER_CONSOLE_*` 环境变量启动完整 HTTP 控制端程序，避免现场部署时重写
  容器 command。
- `signaling-server/signaling_server.py`：一车一驾驶员会话 demo，以及本地 HTTP JSON 信令服务入口。
- `deployments/`：systemd 和容器部署模板。
- `tests/`：设计行为回归测试。

已覆盖的设计语义包括：

- 结构化车端配置校验：配置加载支持 YAML 和 TOML；cloud/ICE/STUN/TURN、急停锁存、时间同步、
  控制阈值、录像容量规划、上传追赶策略和上传 URL/重试时间参数。
- 运行时配置更新门禁：允许日志级别、实时码率、上传限速和上传暂停状态，
  拒绝车辆 ID、证书路径、车辆适配器、相机列表和控制安全阈值热更新。
- 最终生效配置日志摘要：生成可 JSON 序列化的车端配置 payload，并对
  设备证书、密钥和 TURN 凭据做配置状态化脱敏；车端入口启动时会输出
  `effective_vehicle_config` JSONL 记录。
- 公网 cloud URL 安全门禁：`wss`/`https` 为默认要求，只有本机回环
  开发地址允许 `ws`/`http`。
- 公网车端设备身份门禁：非回环 cloud 配置必须声明存在的
  `device_cert`/`device_key`，本地开发配置可继续无证书运行。
- 结构化驾驶端配置校验：cloud、UI 布局、调试 overlay、控制频率、急停长按阈值、
  键盘映射和模拟驾驶器 Gamepad 轴/按钮映射。
- 驾驶端本地操作日志：记录登录用户、连接车辆、会话、控制权、控制命令、断连/重连、急停字段、UI 版本和配置版本，并支持按大小轮转为编号备份文件。
- 驾驶端视频状态模型：每路独立记录连接、fps、码率、延迟、低码率、重连和解码失败状态，并支持 1/2/4 布局、单路放大与布局偏好文件保存；`to_dict()` 输出当前渲染用 `visible_camera_ids`，布局文件只保存 layout、聚焦相机和相机 ID，不持久化瞬时运行状态。
- 驾驶端状态栏快照：把 telemetry、adapter 健康状态、视频面板、丢包样本
  和控制权状态归一化为右侧/底部状态栏字段。
- 驾驶端工具栏快照：固定登录/退出、连接/断开、会话、急停和设置动作，
  并保持急停动作常驻可见。
- 驾驶端输入合成：软件控件、键盘离散量和模拟驾驶器连续轴都进入同一条
  20 Hz 控制命令链路；急停最高优先级，
  刹车覆盖油门，窗口失焦时主动油门和转向归零但继续发送心跳，并在
  本地生成命令前拒绝未知档位。
- V4L2 相机源契约：校验设备路径并生成 GStreamer `v4l2src` source
  fragment；真实相机采集仍需目标 Ubuntu 工控机验证。
- 车端启动 preflight：只读检查启用相机设备、录像目录写权限和指定硬编
  设备节点，报告 ready/missing/not_readable/not_writable；`vehicle-agent
  --preflight` 以 JSONL 输出检查结果，全部 ready/skipped 返回 0，否则返回 2；
  systemd 车端 agent 模板把该检查挂为 `ExecStartPre`，避免缺设备时启动长期
  控制循环。
- 相机源测试能力：支持 TestPatternSource 和 JSONL FileReplaySource，用于无真实相机时复现采集帧序列和单路故障。
- GStreamer/WebRTC 低延迟 H.264 实时管线规划，支持 VAAPI 和 x264 fallback，
  并使用实时 profile 中配置的关键帧间隔。
- H.264 SDP/profile 校验：比对远端 SDP、编码器 `profile-level-id` 和驾驶端解码能力，避免 VAAPI High profile 与 constrained-baseline 驾驶端不兼容却继续推流。
- 媒体 pipeline watchdog：按相机心跳检测媒体线程/管线卡死，首次超时生成 `media_pipeline_stalled` 组件日志，保留控制安全逻辑独立运行的边界。
- 媒体故障恢复决策：单路 pipeline 卡死时请求重启该相机并上报
  `reconnecting`，硬编失败时按配置 fallback 到 x264 并上报 `degraded`，
  两类恢复都生成结构化组件日志。
- 实时连接恢复决策：信令断开请求重连，媒体断开先请求 ICE restart，
  超过次数后请求重建会话，并为每个动作生成组件日志；本地 executor 可绑定
  `reconnect_signaling`、`restart_ice` 和 `rebuild_media_session` 控制器方法。
- 独立录像 H.264 管线规划，使用原分辨率 caps、写盘队列和 `splitmuxsink`
  分段输出，避免和实时 WebRTC 分支混用。
- 控制 DataChannel 的 unordered/unreliable 默认配置，并可通过
  `ControlDataChannelConfig.to_webrtc_init()` 导出浏览器/WebRTC 初始化字段
  `ordered=false`、`maxRetransmits=0` 和协议名。
- 4 路 FFmpeg VAAPI Docker 探测命令、GStreamer 硬编/fallback 插件探测命令，以及 4 路实时、4 路录像、4 路实时+录像同时运行的硬件编码验证计划生成。
- 硬编验证结果归档：`vehicle-media-agent --mode hardware-report` 可读取目标机
  每路 `ffprobe` 输出和系统指标 JSON，生成 JSONL 验收记录，并在 codec、
  分辨率、fps 或码率不达标时返回非 0。
- Mock Telemetry 输出，并明确标记为非真实车辆反馈；每路视频状态输出
  稳定的 state/fps/bitrate/latency/low-bitrate/reconnecting/fault/encoder 字段，
  且系统故障和视频故障会聚合到顶层 `fault_flags`。
- 运维指标快照：车端 CPU/GPU/内存/磁盘/5G/编码/码率/控制超时和
  adapter 健康状态，云端信令/TURN/会话/上传成功率/失败原因，以及驾驶端
  解码 fps、控制发送频率、UI 卡顿、RTT 和丢包率。
- 控制验收指标报告：记录控制发送/接收频率、RTT、乱序/过期命令计数、
  超时触发时间、开始制动前滑行时间/距离、分级制动反馈样本和总停车距离，
  供后续场地实测归档。
- 视频验收指标报告：按相机记录 fps、码率、端到端延迟、丢帧率、解码失败和重连次数。
- 录像验收指标报告：按相机记录分段完整性、sidecar 元数据完整性、文件大小、编码 fps、写盘延迟和磁盘占用增长。
- 上传验收指标报告：记录上传速度、重试次数、失败原因，以及上传期间实时流 fps/码率相对基线的变化。
- 统一验收指标汇总：`scripts/acceptance_metrics_report.py` 可把场地或弱网
  JSONL 样本转成视频、控制、录像和上传四类归档报告，并在汇总和每类报告顶层输出
  `passed/failures`，供目标主机归档校验拒绝显式失败样本。
- 组件运行日志 JSONL：固定 `ts/level/component/vehicle_id/session_id/camera_id/event/message/error_code`
  字段，供运维排障和日志聚合使用，并支持运行时日志级别过滤和按大小轮转为编号备份文件。
- JSONL 审计日志，用于会话创建/结束、控制权授予/回收和急停等关键事件，并支持按大小轮转为编号备份文件。
- 云端急停审计：信令服务可记录会话级 `estop`，要求当前参与者凭据并保存触发原因和控制序号。
- 云端异常断开审计：信令服务可记录 `abnormal_disconnect`，包含会话、
  车辆、参与者、断开原因和检测组件。
- 云端实时诊断审计：当前会话参与者可上报 RTT、丢包率、抖动、视频延迟和
  控制发送频率，服务校验对应 token 后记录 `realtime_diagnostics`。
- 云端控制超时审计：车端可上报最后有效控制接收时间、进入超时制动时间和
  配置阈值，服务校验设备 token 后记录 `control_timeout`。
- 车辆下线审计：Upload/Signaling 参考服务可记录 `vehicle_offline`，
  并把受影响的活跃会话标记和审计为 `session_failed`。
- TURN 中继审计与用量统计：信令服务可记录 `turn_relay_enabled` 和
  `turn_relay_usage`，包含 TURN URL、relay candidate、selected pair、
  relay bytes 累计量和最近样本带宽；内部 coturn `usage` 日志解析器可把
  `username=<session_id:actor>`、`rb`、`sb` 和 `duration_ms` 转成同一套用量审计；
  `scripts/coturn_usage_report.py` 可生成脱敏 JSONL 用量验收报告。
- ICE/STUN/TURN 配置发放：`signaling-server --vehicle-config` 可读取车端
  `ice` 配置，`/sessions/{session_id}/ice_servers` 只向当前会话驾驶端或车端
  发放 WebRTC `RTCIceServer` 形态列表，并审计发放数量而不记录 TURN credential；
  TURN 可使用静态 password，也可使用 coturn `use-auth-secret` 的 REST-style
  临时 credential，按会话参与者生成过期用户名和 HMAC-SHA1 密码。
- 车端控制服务审计 hook：实际进入控制超时制动、急停锁存或本地确认复位时可写 `control_timeout` / `estop_latched` / `estop_reset` 事件，并避免重复刷屏。
- 车辆适配器工厂和真实接口契约门禁：当前只允许 `mock` 直接启动；配置为 `can` 或 `dynamic_library` 时必须声明控制单位、档位、心跳、安全能力、确认方式和 telemetry 字段。`can` 与 `dynamic_library` 都可在 `abi: c_shim` 且声明 `bridge_library_path` 后通过 ChassisControl bridge 运行；该配置记录 ChassisControl 动态库、CAN C ABI 头文件和 MinePilot 低层 CAN、`can_db`/receiver/sender 头文件与源码来源。仓库提供 `deployments/chassis-control-bridge/` C shim 模板和 `mine_teleop_chassis_bridge.h` 稳定 ABI 头，链接 MinePilot 提供的 `libchassis_control` 时会匹配 MinePilot API 头并通过 `mine_teleop_chassis_poll_feedback` 拉取 decoded CAN 反馈，避免真实车辆联调阶段静默退回 Mock 或只靠 Mock telemetry。
- ChassisControl/MinePilot bridge 前置检查：`python3 scripts/chassis_bridge_check.py`
  会以 JSONL 验证 `/Volumes/SystemDisk/Workspace/ChassisControl`、
  `/Volumes/SystemDisk/Workspace/MinePilot`、MinePilot 低层 CAN 头文件、
  `can_db`/receiver/sender 头/源文件、动态库和
  bridge CMake configure；默认要求 ChassisControl 在 `UI_Test`、MinePilot 在
  `merge_ui_test`，并记录两个 checkout 的 HEAD commit 与 dirty 状态；显式加
  `--build` 时还会执行 bridge target 构建。
  本机可验证 configure；macOS 开发机可用 `--docker-command` 生成 Linux/amd64
  容器构建命令，目标 CAN 主机仍需执行真实 build 与台架联调。
- ChassisControl 车端配置生成：`python3 scripts/render_chassis_vehicle_config.py`
  可从 dev 配置渲染目标 `vehicle-agent.yaml`，注入 C shim bridge library、
  实际链接的 ChassisControl 动态库、MinePilot CAN DB/receiver/sender 路径和
  控制超时标定证据，减少联调时手工拼接真实 adapter 配置的风险。
- 目标主机验证计划：`python3 scripts/target_host_validation_plan.py` 可生成
  JSONL 或 shell 形式的验收命令清单，覆盖 GPU/DRI、VAAPI、车端 preflight、
  CAN interface、MinePilot CAN source/socket/send 探针、ChassisControl bridge、
  VehicleAdapter 打开/健康状态 smoke、decoded CAN feedback poll、弱网矩阵、硬编结果归档模板和统一验收指标报告模板；带 `--artifact-dir` 的 shell 会自动写出
  `target_host_validation_archive.jsonl`，其中 `target_host_validation_report.py --verify-artifacts`
  会核对 summary/result 计数、命令清单、附件完整性并解析 feedback poll stdout；
  summary 同时记录 vehicle config、CAN interface、外部 checkout 路径、
  ChassisControl/MinePilot 分支、bridge shim 路径、底层 ChassisControl 动态库路径、
  commit 和 dirty/changed_paths 状态，缺少
  `vehicle_adapter_feedback_poll.received=true` 时即使命令返回 0 也判定归档验收失败。
- 真实车辆控制超时标定门禁：非 `mock` 车辆适配器配置必须声明 `control.timeout_calibration` 证据，且 `control_timeout_ms` 不能超过标定上限，避免用未标定网络体验值进入真实车辆联调。
- 上传队列持久化、暂停/恢复、预签名 URL 过期刷新、失败原因记录和
  指数退避重试。
- 上传限速：按配置的 `max_bandwidth_mbps` 对逐文件上传调度做节流，
  限速等待时保持片段为待上传状态，不阻塞控制循环。
- 上传网络质量暂停策略：根据连接状态、RTT、抖动、丢包和上行带宽样本
  输出暂停/恢复决策，可驱动上传队列 `pause()`。
- 实时流码率自适应策略：RTT 或丢包超阈值时按比例降码率，网络恢复后
  逐步回到目标码率；本地 runtime controller 会把允许的实时 profile
  码率更新转成命名 encoder 的 `bitrate` property update，并可通过
  GStreamer pipeline property setter 绑定到命名元素；弱网更差时也可按
  预声明实时 profile 从 720p30 下切到 480p15，并在 pipeline hook 成功后
  更新活动状态；真实
  GStreamer/WebRTC 主循环仍需目标 Ubuntu 工控机端到端验证。
- 上传 backlog 告警：运行期统计未上传片段大小，当上传能力低于录像产生速率时返回配置的滞后策略动作，避免队列无限增长却没有状态信号。
- 上传触发策略：按片段数量、累计字节、时间窗口或网络空闲触发调度，
  上传单位仍保持单个视频片段和 sidecar。
- recorder/uploader 闭环：为视频片段和 sidecar 元数据分别申请上传凭证，
  按 `cameras/` 与 `metadata/` 对象路径归档并分别登记上传结果，且本地归档适配器拒绝写出归档根目录的对象路径。
- 本地信令服务：健康检查、车辆上线、驾驶端登录、会话创建/结束、
  驾驶端短期 bearer token 过期校验、每车设备 token 注册、会话控制令牌签发、一车一驾驶员控制权、
  独立控制权回收 API、会话参与者校验、HTTP JSON 和最小 WebSocket 信令消息排队/拉取、
  且信令发送/收件人/拉取/连接必须是当前会话参与者，发送/拉取/连接必须带对应驾驶员 token 或车端设备 token，审计落盘。
- 信令服务 TLS 边界：`signaling-server --serve` 默认仅回环明文开发；显式
  绑定非回环地址时必须同时提供 `--tls-cert` 和 `--tls-key`。
- 本地 Upload API：设备 token 校验、片段上传凭证签发、同一对象路径续签、
  上传对象路径段安全校验、上传成功/失败登记和审计落盘、上传成功字节数非负校验，并从对象路径恢复车辆/会话归属；对象路径车辆
  与 payload 车辆不一致时拒绝跨车辆登记，非标准对象路径也会被拒绝。
- 驾驶端命令生成：20 Hz 全量状态、窗口失焦安全心跳、键盘急停长按门控、急停立即发送和冗余发送。
- 控制接收：协议/车辆/会话/会话控制令牌/控制权/序号/本地到达间隔校验，
  普通控制命令超过本地到达间隔会被拒绝；急停命令仍需通过身份、会话、令牌和序号校验，但不会被普通心跳间隔拒绝。对明显驾驶端时间戳偏差输出 warning 而不是直接拒绝控制。
- 时间同步启动记录：解析 chrony tracking 输出，评估 NTP/PTP 最低要求、
  当前偏差估计和同步状态，并以组件日志字段输出启动状态。
- 弱网测试基线：固定文档要求的延迟、抖动、丢包和带宽档位，并提供
  dry-run `tc netem` 命令生成脚本。
- 车端控制服务循环：接收控制命令、驱动安全状态机、只在控制有效时下发配置的 VehicleAdapter、周期输出带 adapter 打开/健康状态、控制下发计数和安全停车计数快照的 Telemetry、断连后本地进入降级/超时制动、信令断开不立即改变本地控制状态、急停锁存。
- 车端媒体/控制 IPC 边界：提供 latest-only 本地命令 mailbox，媒体侧连续写入时只保留最新控制状态，避免旧命令在本地通道积压。
- 车端 recorder/uploader 循环：写入视频片段和 sidecar、持久化上传队列、刷新上传凭证、上传到本地归档适配器、登记上传成功/失败，上传失败进入退避且不阻塞控制循环。
- systemd 服务模板：vehicle agent、media agent、uploader 和 signaling server，
  以及独立 TURN server，覆盖固定环境、自动重启和文件日志路径。
- 容器部署模板：固定 Ubuntu 22.04 媒体栈、挂载 `/dev/dri`、相机设备和
  录像目录，并为车端媒体/上传/TURN 进程设置资源限制。

本地检查：

```bash
python3 scripts/check.py
```

运行开发入口：

```bash
python3 vehicle-agent/vehicle_agent.py
python3 vehicle-agent/vehicle_agent.py --config configs/vehicle-agent.dev.yaml --adapter-status
python3 vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml --adapter-status --poll-feedback --require-feedback
python3 vehicle-media-agent/vehicle_media_agent.py --mode pipeline
python3 vehicle-media-agent/vehicle_media_agent.py --mode vaapi-probe
python3 vehicle-media-agent/vehicle_media_agent.py --mode gst-probe
python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-probes
python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-report --scenario four-camera-realtime-720p30 --ffprobe-output front-realtime-720p30=/tmp/front.ffprobe.txt --metrics-json /tmp/mine-teleop-vaapi-metrics.json
python3 vehicle-uploader/vehicle_uploader.py --work-dir .local/uploader-demo
python3 vehicle-uploader/vehicle_uploader.py --service-mode --process-once --config configs/vehicle-agent.dev.yaml --work-dir .local/uploader-service --upload-api-base-url http://127.0.0.1:8765
python3 driver-console/driver_console.py --config configs/driver-console.dev.yaml
python3 driver-console/driver_console.py --config configs/driver-console.dev.yaml --operation-log .local/driver-ops.jsonl --operation-log-max-bytes 10485760 --operation-log-backup-count 5
python3 -m mine_teleop.control_console_container
python3 signaling-server/signaling_server.py --serve --host 127.0.0.1 --port 8765 --audit-log .local/signaling-audit.jsonl --audit-log-max-bytes 10485760 --audit-log-backup-count 5
python3 scripts/render_chassis_vehicle_config.py --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so --max-control-timeout-ms 900 --calibration-evidence bench-brake-test-2026-06-24
python3 scripts/target_host_validation_plan.py --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so --format shell
```

到工控机现场联调前，先按
[当前实现与工控机无 Docker 部署](docs/14-current-status-and-ipc-deployment.md)
收束部署包、动态库、车端配置和验收附件；当前主路径不要求 Docker。需要生成
“Ubuntu 执行文件 + 动态库”交付包时，使用
[Ubuntu Bundle 使用说明](docs/16-ubuntu-bundle-usage.md) 中的
`scripts/build_ubuntu_bundle.py` 流程。

生成弱网测试 dry-run 命令：

```bash
python3 scripts/netem_plan.py --interface wwan0 --delay-ms 100 --jitter-ms 20 --loss-percent 3 --bandwidth-mbps 10
python3 scripts/netem_plan.py --interface wwan0 --matrix
```

该脚本只打印 `tc netem` apply/clear 命令，不会修改系统网络配置；`--matrix`
会按测试文档生成 54 组延迟/抖动/丢包/带宽组合。

查看容器部署模板：

```bash
sed -n '1,220p' deployments/container/docker-compose.vehicle.yml
sed -n '1,120p' deployments/container/Dockerfile.media
```

运行车端长期控制服务模拟：

```bash
python3 vehicle-agent/vehicle_agent.py --run-loop --duration-ms 1500 --disconnect-at-ms 500
```

启动本地信令服务：

```bash
python3 signaling-server/signaling_server.py --serve --host 127.0.0.1 --port 8765 --audit-log .local/signaling-audit.jsonl
```

当前服务默认注册开发车辆凭据：`vehicle-001` 的设备 token 为
`dev-device-secret`，驾驶端登录密码为 `dev-password`。服务支持继续注册
其他车辆的独立设备 token；这些仍用于本地闭环和接口验证，不是生产证书方案。
联调时可给 `signaling-server` 增加 `--driver-credentials <json>` 和
`--device-credentials <json>`，用 PBKDF2-SHA256 驾驶员凭据文件和车辆 token
文件替代默认开发凭据。

开发 Upload API 端点：

- `POST /uploads/credentials`：为视频片段或 sidecar 元数据签发上传 URL。
- `POST /uploads/complete`：登记上传成功。
- `POST /uploads/failed`：登记上传失败和失败原因。

当前 Upload API 默认签发本地可验证的占位 URL，用于固定对象路径、过期时间和刷新语义；代码中也提供 S3-compatible SigV4 预签名 PUT 生成器。`signaling-server --vehicle-config` 会读取 `upload.backend=s3` 和 `upload.s3` 目标配置来使用真实 endpoint、bucket、region 和凭据；车端 uploader 在 `upload.backend=s3` 时会把视频和 sidecar 直接 HTTP PUT 到签发 URL，再登记上传成功；`scripts/upload_presign_report.py` 可离线输出脱敏的签名验收 JSONL。目标厂商签名兼容性、凭据轮换和权限策略仍需在部署环境中联调。

生产级 WebSocket/TLS 证书部署、真实 TURN 可达性、Qt/GStreamer/WebRTC、4 路
VAAPI 并发和真实车辆适配仍需要在目标硬件和依赖环境上继续接入验证；当前
实现先提供可测试接口边界和 Mock 闭环，避免在硬件未齐备时把安全状态机、
信令和上传队列语义悬空。
