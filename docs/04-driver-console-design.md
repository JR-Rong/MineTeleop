# 驾驶端 Console 设计

## 职责

Driver Console 运行在远端模拟驾驶器上，负责：

- 用户登录。
- 选择车辆或会话。
- 建立实时连接。
- 显示多路视频。
- 产生控制命令。
- 显示车辆状态和链路状态。
- 处理急停。
- 记录本地操作日志。

## 应用形态

当前实现采用便携 C++20 本地进程与系统浏览器组合：C++ 进程只监听
`127.0.0.1`，保存登录/会话/token、维护最新输入租约，并以固定 20 Hz 通过独立
控制 WSS 生成和发送完整控制命令；浏览器负责 WebRTC 解码、键盘和标准 Gamepad
API，并通过 DataChannel 交换 session profile、VCU 握手和状态。浏览器不生成或
经 DataChannel 发送普通 `control_command`。平台差异仅保留默认浏览器启动、动态库
和文件路径适配。

这个形态让 macOS、Windows 和 Ubuntu 共享同一套核心与页面，同时不把长期设备
凭证或 TURN 密钥放进浏览器。特殊方向盘、踏板和档位器如果不能通过标准 Gamepad
API 表达，应放到独立平台适配器中，不能侵入会话或协议核心。

## UI 布局

当前布局：

- 顶部：驾驶员登录、授权车辆、连接/退出与常驻急停；
- 输入区：Gamepad 名称、映射类型、归一化值和中心/量程校准；
- 监控区：车辆、会话、控制权、编码/后端、RTT、ICE 路径、TURN 和时间同步；
- 视频区：每个 `camera_id` 一个独立播放器；
- 逐路指标表：FPS、码率、丢包和端到端时延；
- 调试区：保留原始指标 JSON，不作为驾驶员的主要判断入口。

不要在主画面堆叠说明性文字。遥操界面应优先可扫视、低干扰、状态清楚。

## 控制输入

当前输入：

- 键盘。
- 标准映射 Gamepad。
- 可配置轴的非标准方向盘/踏板。

固定映射（不从 YAML 读取或覆盖）：

- `ArrowUp` / `W`：前进目标车速比例。
- `ArrowDown` / `S`：倒车目标车速比例。
- `ArrowLeft` / `A` 与 `ArrowRight` / `D`：转向。
- `Space`：缓刹，选择当前会话配置的每路 EHB 缓刹压力。
- `B`：急刹，立即选择当前会话配置的每路 EHB 急刹压力。
- `E`：立即锁存急停。

注意：

- 键盘控制必须有回中/回零策略。
- 失去窗口焦点或页面隐藏时，浏览器应立即向本机进程提交零执行量意图；即使浏览器
  随后被冻结，原生发送线程也继续固定周期的安全心跳，不应以网页停发制造超时急刹。
- 急停取键盘、页面按钮和 Gamepad 的并集，始终最高优先级。
- 键盘离散输入覆盖同一时刻的 Gamepad 连续量；相反方向同时按下时输出归零。
- 急刹优先于缓刹；任一刹车输入存在时目标车速比例输出为 0，转向输入保持独立。
- 前进/倒车键选择的 `D`/`R` 在当前控制会话内锁存；松开按键只将目标车速比例归零，普通刹车不得把挡位切回 `N`。
- 任何新的 `D`/`R` 选择（包括 `N`/`P`→`D`/`R` 以及 `D`↔`R`）都必须有 VCU 有效且绝对值不大于 `0.1 m/s` 的零速反馈；唯一例外是明确不支持 VCU 握手的 mock。键盘和 Gamepad 必须调用同一挡位 reducer；门禁未满足时目标车速比例保持 0，并提示驾驶员停车后释放、重新操作。
- 多个物理键映射到同一方向时必须按键码集合派生状态；例如同时按住 `ArrowUp` 和 `W`，释放其中一个不能清除前进状态。
- 首次获得 VCU Ready 前不得记录运动按键；只有当前会话曾经 Ready 后的 `wait_gear` /
  `wait_actuator_modes` 闭环等待才保留按键集合。此时页面只刷新目标车速比例强制为 0
  的输入意图，完整安全快照仍由原生线程按 20 Hz 生成。
- 真实链路中断、控制权丢失、VCU fault 或 disarm 必须清空输入并重置 Ready 资格，要求释放后重新按键，禁止旧按键自动恢复。
- Gamepad 在首次 Ready、链路/控制权重置或窗口失焦后进入踏板回零互锁；只有观测到油门与刹车都回零后，后续的新踏板动作才可生效。
- 窗口失焦时主动目标车速比例和转向归零；安全心跳的固定频率由原生进程维持。
- 档位输出必须落在控制协议允许集合内；真实车辆额外档位由后续车辆适配器契约扩展。

后续输入适配：

- USB HID 方向盘。
- 踏板。
- 档位器。
- 自定义串口/CAN 驾驶台。

浏览器读取 Gamepad 后先执行死区、反向、中心/静止位和量程归一化，再与键盘
合成为 `steering`、`throttle`、单一 v1 `brake`、`estop` 和 `gear`。协议字段
`throttle` 为兼容保留名。控制端把踏板比例乘以已确认的会话目标车速，再除以车端
`field_safety.max_speed_kph`，车端仍按 `throttle × max_speed_kph` 得到 PID 目标。
该比例不是转矩上限；PID 输出同时受已确认的会话单电机转矩上限与车端
`field_safety.full_scale_motor_torque_nm` 限制。

会话中的三项制动值是每路 EHB 物理压力请求，单位 `bar`、分辨率 `0.1 bar`，不是
百分比、踏板行程或整车制动力。v1 wire `brake` 保持 `[0,1]`：缓刹和急刹分别发送
`service/max`、`hard/max`，模拟踏板按最大普通压力线性映射；最大压力为 0 时三种普通
制动输入都发送 0。车端按已确认的会话最大压力还原为 bar，并继续受车端硬上限截断。
标准映射手柄使用
左摇杆 X 与左右扳机；非标准设备由 `control.gamepad` 的轴配置控制。页面校准仅对
本次运行有效，需长期保留的现场校准值应回写 YAML。设备不存在、映射不完整或
断开时，Gamepad 分量必须全部归零。

## 控制命令生成

浏览器只维护输入意图，不能承担车辆发包时钟。输入变化时以及输入租约有效期内，
页面向回环接口 `POST /api/control-intent` 提交 latest-only 完整意图；原生 Console
独立采样该意图并生成车辆控制命令。

默认：

- 活动会话内的原生发包频率：20 Hz；不受浏览器 timer、渲染、焦点或可见性调度。
- 每条命令包含协议版本、车辆、驾驶员、会话、单调递增 `seq`、
  `sent_at_utc_ms`、短期 `control_token`、`intent_seq` 和 `intent_fresh`。
- 每条命令包含完整控制状态，而不是增量。
- 命令经专用 send-only 控制 WSS 进入 signaling 的容量 1、TTL 150 ms latest-only
  mailbox，再由车端独立 control-only WSS 接收；普通命令不走 WebRTC DataChannel。
- loopback C++ 运行时在控制权租约签发后约 1/3 处，用当前驾驶员 token
  调用服务端续租；续租保持 session、控制 WSS 与 DataChannel profile 中的
  `control_token` 不变，
  不把任一 token 暴露给浏览器 JavaScript。

浏览器输入租约默认 200 ms，可配置范围为 100--1000 ms。失焦/隐藏时页面主动提交
零执行量；页面冻结、更新超时或控制 WSS 异常时租约会过期/失效，原生进程保留已选
挡位并把转向、油门和普通制动精确归零，同时标记 `intent_fresh=false`。车端只在
`CONTROL_ACTIVE`、profile 与 VCU 握手仍有效、距上一条已接受命令尚未达到
`degraded_timeout_ms`、且挡位等于最后一条新鲜非急停命令的挡位时，接受这种零值包
维持看门狗；它不能建立或恢复控制。任何过期非零指令都丢弃，原生进程/WSS 真断流
仍触发 `DEGRADED`/`TIMEOUT_BRAKE`，恢复前必须先提交新鲜中立意图。
安全退出、关闭页面触发会话结束，或原生控制进程退出时不会继续发包；这时车端按
权限撤销或真实包间隔进入本地安全停车。

### 会话控制参数确认

控制端 YAML 给出新会话的驾驶参数默认值：目标车速 `2.0 km/h`、单电机最大驱动转矩
`300.0 Nm`、每路 EHB 最大普通/缓刹/急刹压力 `100.0/30.0/100.0 bar`，以及最大转向角。
`300 Nm` 和 `100 bar` 只是未完成台架或实车标定的软件请求默认值。控制端 schema 上限为
`640.0 Nm/路` 与 `327.6 bar/路`，实际可用值还必须被车端 hard limits 下调。

速度 PID 默认值不在控制端 YAML 或 JavaScript 中保存。页面只有在车端 `control_limits`
同时上报 `default_speed_pid_kp/ki/kd`、`default_speed_pid_derivative_filter_tau_ms`、
`default_speed_pid_max_dt_ms` 以及嵌套 `speed_pid_limits` 的五组 min/max 后，才初始化
PID 表单。车端还必须同时上报顶层 `speed_feedback_timeout_ms`、
`hard_overspeed_margin_kph` 和 exact `read_only_control_safety` 对象；对象包含固定 20 Hz
上游命令频率、命令间隔、degraded/control watchdog、减速曲线、速度反馈超时、超速余量、
CAN/本地急停/时间同步门禁及 commissioning mode。重复的速度超时和超速余量必须与顶层
数值相同。页面只读展示原始 `max_speed_kph`、`max_throttle`、派生目标车速和上述固定值；
缺少、越界、类型/字段错误或重复值不一致都撤销驾驶授权，不能由会话 profile 修改。

首次应用 profile、任一 PID 修改、提高目标车速/转矩、转向上限任意变化，以及修改任一
制动压力字段（包括降低），都要求 N 挡、有效零速、电子驻车已拉起且 VCU 状态为
`standby` 或 `disarmed`；明确不支持 VCU 握手且 `adapter_ready=true` 的隔离 mock 台架
例外。控制端先预检，车端再次 fail closed。

页面通过 `POST /api/control-profile` 准备 `type=session_control_profile` 的鉴权
DataChannel envelope。V3 profile 在 envelope 顶层包含 `profile_version=3`、目标车速、
单电机最大转矩、三项普通制动压力、最大转向角，`speed_pid_kp/ki/kd`、
`speed_pid_derivative_filter_tau_ms`、`speed_pid_max_dt_ms`，以及
`motor_torque_rise_rate_nm_per_s`；它与普通控制命令复用车辆、
驾驶员、session 和 `control_token`，但使用 DataChannel 自己的消息序号。由于该
profile/status DataChannel
是 unordered/unreliable，浏览器每隔至少 200 ms 重发同一 envelope 和同一 `seq`，直到
收到共享 `control_status_seq` 排序后的 `session_control_profile_status`，或 telemetry
中的同一 canonical `session_control_profile`。只有 `active=true`、`accepted=true`、
`last_request_seq` 匹配 pending 序号、顶层正整数 `applied_revision` 等于该请求 `seq`，且
`effective_profile` 的全部 V3 字段和值与请求精确一致时才视为已确认；同一 revision 的
幂等重 ACK 可保持授权。
旧 `GET /api/control-limits` 仅保留归一化制动比例的只读兼容；`POST` 固定返回
`410 Gone`，不能绕过 profile 的停车、鉴权和 ACK 门禁修改会话制动参数。旧的
`POST /api/control`、`POST /api/control/keyboard` 与 `POST /api/control/gamepad`
同样固定返回 `410 Gone`：浏览器和新集成都必须改用 `/api/control-intent` 更新本机
latest-only 意图，不能再准备或自行发送 v1 命令。

未确认时页面清空输入并禁止 VCU connect 握手和所有普通驾驶命令；`estop=true` 不受
profile ACK 门禁阻止。后续 telemetry 报告 profile inactive/rejected、请求序号或
`applied_revision` 不再等于当前 effective revision、effective profile 非法或任一字段
变化时，页面立即撤销确认并再次清空输入。Peer/DataChannel 断开、切换车辆、会话结束或
登录失效会清除 requested profile、pending envelope、effective profile、revision 和 hard
limits；新会话必须重新等待车端 PID 默认值，不得跨会话自动重放旧设置。
默认 profile 仅在收到 `parking_ready=true` 且 VCU 为 `standby`/`disarmed`，或车端明确
报告 mock/无需握手且 adapter ready 后自动提交；每个链路 generation 最多自动尝试一次。
车端拒绝后必须由驾驶员显式重试，不得借后续 telemetry/hard-limit 更新循环生成新 `seq`。

按键集合、挡位门禁、刹车优先级、VCU 状态迁移、状态序号与
latest-wins 判定由可在浏览器和 Node.js 共用的无 DOM 模块实现。生产页面
实际调用该模块；Node.js 测试执行行为矩阵，C++ 页面测试只核对 wiring。

`POST /api/control-intent` 返回只表示本机 C++ latest-only 意图存储已接受或刷新该
`intent_seq`，不表示原生 WSS 已发送、signaling 已接收或车端已接受。请求必须携带
当前 `session_id`、`session_generation` 和 `ui_instance_id`；旧会话的延迟成功或失败
响应不能修改新会话状态。过期序号、同序号异内容、跨 UI 实例的非中立起步以及
租约/传输失效后的非中立输入均 fail closed；ESTOP 在当前会话内锁存并可抢占普通
意图。原生命令序号由 C++ 进程独立生成，和浏览器 `intent_seq` 不是同一个序列。

车端若在实际 apply 阶段拒绝命令，会发送带共享 `control_status_seq` 和原命令
`command_seq` 的 `control_command_rejected`。页面只解释本地 allowlist 中的
`issue_code`，不展示车端异常文本；换挡移动/反馈过期时，页面用
`command_seq` 关联当前换挡尝试，立即清空按键与 Gamepad 输入、恢复到拒绝前已选挡位，
并发送该挡位的零牵引快照；不再自动回 N。按住的方向输入必须物理释放后才能重试。
车端同时锁存该次拒绝；即使车速随后降到零，持续发送旧目标挡也不会自动生效，只有
拒绝前挡位的零牵引（或制动）帧才能解除该次门禁。
无法关联到当前事务的换挡拒绝不会猜测或重发挡位，而是冻结普通控制，保留急停和
显式断开，并要求完成安全断开后重新握手。
相同拒绝由车端限频重发，以覆盖 unordered/unreliable 状态 DataChannel 的单包丢失，
同时避免拒绝风暴。

## 视频显示

要求：

- 每路独立连接状态。
- 每路可显示低码率/断流/重连状态。
- 支持单路放大。
- 支持布局保存。
- 解码失败不能拖死整个 UI。

当前控制端会把车端 `webrtc_offer` 返回给浏览器页面，
页面用 `RTCPeerConnection` 创建 answer，并通过 `ontrack` 把远端视频流挂到对应
camera 的 `<video>` 元素；profile/VCU/status DataChannel 按 unordered/unreliable
配置创建，不承载普通控制命令。
视频轨道不以 VCU 握手或 CAN adapter ready 为显示前提：浏览器收到 `ontrack`
就立即挂载画面。车端只在控制 DataChannel 打开后启动 VCU adapter；adapter
启动或运行失败时继续保留视频，把握手状态上报为 `fault`，随后只关闭控制
DataChannel 并阻止所有驾驶命令。关闭控制通道也保护不认识 `adapter_ready`
字段的旧控制端，避免其误以为软件急停已经送达。页面把点击动作显示为“急停
请求已锁定”，只有收到车端 `estop=true` 遥测后才显示车辆急停已确认；否则
明确要求使用车辆物理急停。配置文件自身
不合法、身份/信令失败或相机/编码链失败仍会阻止媒体启动。
页面连接后周期读取本机 C++ 运行时的消息缓冲并处理车端 remote ICE candidate；
跨网信令本身使用经证书校验的 WSS push/ack，不再使用 HTTPS 消息轮询。浏览器
local ICE candidate 经本机 C++ WSS 客户端转发到车端。服务端推送带单调
`delivery_cursor`，本机 C++ 先保存再确认；未确认消息在同会话 WSS 重连后补发，
客户端按游标去重。控制端发出的信令带稳定 `message_id` 和序列号，ACK 不确定时
用原消息自动重试，服务端只入队一次。本地 Docker smoke 只验证 offer/answer/ICE
信令和页面 wiring，不替代真实车端 RTP 媒体流与 DataChannel 端到端现场验证。
切换车辆时页面先停止旧控制循环并关闭旧 PeerConnection，C++ 运行时必须收到旧
会话结束确认后才允许为新车辆创建会话；释放失败时保持故障态，不能忽略错误后
同时持有两辆车的控制权。仅结束会话使用 `/api/end-session`，保留驾驶员登录；
安全退出则撤销登录并释放全部控制权。每代本地消息读取循环带 generation，旧循环不能
在新会话建立后重新进入。
安全退出和单独结束会话都必须先停止本地实时控制，再等待服务器确认撤销；服务器
不可达时不能在 UI 中显示“已安全退出”，也不能丢弃用于重试释放的本地会话状态。
此时车辆仍必须依靠车端超时状态机进入安全制动，服务端再由心跳超时回收控制权。
页面状态卡直接显示 session、控制权、编码/后端、控制 RTT、Direct/STUN/TURN、
TURN 使用状态和时间同步可信度。逐路指标超过 200 ms 或低于 20 FPS、控制通道
中断、控制权丢失、急停锁存或时间不可信时必须显示明确告警。

## 安全交互

急停必须显眼且容易触达。

建议：

- UI 急停按钮常驻。
- 键盘急停需要避免误触。
- 触发急停后，驾驶端立即发送 `estop=true`，并在连接仍可用时重复发送若干次作为冗余。
- 车端收到一次 `estop=true` 即锁存进入 `ESTOP`，不依赖驾驶端持续发包维持急停。
- 急停复位不应只靠驾驶端按钮，真实车辆应有现场安全确认机制、授权人和审计记录。

## 日志

本地日志至少包含：

- 登录用户。
- 连接车辆。
- 会话开始/结束。
- 控制权获取/释放。
- 急停。
- 断连。
- 重连。
- UI 版本。
- 配置版本。

当前 C++ 控制端把页面 UTC 事件通过回环接口 `/api/browser-event` 写入 JSONL，
路径、单文件上限和保留文件数分别由 `logging.browser_event_log`、
`logging.browser_event_log_max_bytes` 和 `logging.browser_event_log_files` 配置。
达到上限后生成 `.1`、`.2` 等编号备份；文件数包含当前文件。键名包含
`password`、`token`、`secret` 或 `credential` 的值会在写盘前递归替换为
`[redacted]`，单条超出文件上限的事件会被拒绝。日志用于本地排障，不能替代服务端
会话审计或车辆安全记录。

现场排查可显式开启 `logging.control_trace_commands`。页面约每秒批量记录
`control_trace_batch`，其中 timer/queue 计数只描述浏览器刷新输入租约，不代表 20 Hz
车辆发包。`/api/status.native_control` 才提供原生 WSS 的连接、重连、发送/失败总数、
最近/最大命令间隔以及发送/ACK 序号。车端 `vehicle_control_trace_batch` 记录实际收到
的原生命令、`intent_seq`/`intent_fresh`、处理耗时和拒绝原因。三者均按原始 session
作用域关联且不记录 `control_token`；signaling latest-only mailbox 允许中间命令被
覆盖，不能把每个浏览器意图或原生发送序号都当作车端必达确认。
