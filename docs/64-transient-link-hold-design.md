# 64 瞬态链路保持设计

状态：**已实现，待现场验收**（设计 2026-11-04 逐节确认，2026-11-05 实施完成；提交范围见 `git log`，第 14 节记录实施中与设计不同的地方）。适用分支：`rjr/cx/native-realtime-pr26`（PR #26）。

## 1. 背景与目标

### 1.1 问题

直连（P2P DataChannel）或中转（云端 WSS、TURN）链路上的临时延迟尖峰，会触发
"安全停车"：全制动并要求驾驶员松键重按（制动等待），严重时硬超时撤销会话配置。
历史证据：

- session-000008：41 次制动等待中 33 次由视频引起（[45](45-control-stop-trigger-review.md)）。
- session-000076：80 次制动等待中 72 次由视频引起；驾驶端选中 TURN/TCP，
  采样往返 412 ms（[41](41-ack-braking-recovery-and-session76.md)）。
- PR #26 的 Windows/Orin 5G mock 长测多轮在约 1,040–1,640 秒之间因控制链路制动中止
  （PR 说明与 `docs/evidence/`）。

### 1.2 目标

- 控制链路总中断不超过 1000 ms、视频停顿总时长不超过 1200 ms 的瞬态，不再触发
  安全停车；直连和中转一致适用。
- 瞬态期间车辆保持安全：不施加牵引，车速不因坡道上升。
- 链路恢复后自动继续，不需要松键重按。
- 真实断链和故障仍按现有保护停车；真断网时的制动不晚于现状。

### 1.3 非目标

- 不放宽"新鲜"的定义：命令单向年龄上限 200 ms、视频新鲜度门限 500 ms 都不变。
- 不改输入租约（浏览器或桌面卡顿）和校时规则。
- 不改传输选路：中转时控制继续走云端 WSS；不启用 TURN 上的控制 DataChannel，
  不做"最快健康路径"切换。
- 不做多路径冗余：PR #26 的 DC/WSS 镜像按决定删除。
- 不改视频传输本身（FEC、拥塞控制等）。

## 2. 现状：尖峰如何变成停车

| 触发点 | 位置 | 当前门限 | 结果 |
|---|---|---|---|
| 命令单向年龄 | `ControlReceiver::accept` | 超过 200 ms 丢弃；连续迟到达到会话配置上限（默认 3，可配 1–5） | 制动等待 + 重按（`core.cpp:3484-3492`） |
| 有效命令间隔 | `SafetyStateMachine::tick` | ≥ 300 ms 进入 `DEGRADED`（撤牵引、保持转向） | 链路恢复后的第一包触发制动等待 + 重按（`core.cpp:3477-3480`） |
| 驾驶端 ACK 往返 | `NativeControlAckRecovery` | 连续 3 次超过 200 ms，或 500 ms 无进展 | `control_link_ready=false`、制动修订号变化，车端制动等待 |
| 驾驶端 DC 往返 | `control_dc_ack_recovery_` | 同上 | 切回 WSS；代际变化要求中性，车端制动等待（`webrtc_media.cpp:2006-2009`） |
| 驾驶端画面 | `camera_view.js::criticalHealth` | 呈现帧 ≥ 500 ms 未更新；连续 3 次异常递增制动修订号 | `critical_video_ready=false`，制动等待 + 重按 |
| 车端编码帧 | `enforce_control_recovery` | 帧年龄 ≥ 500 ms；连续 3 次丢帧递增该路制动修订号 | 制动等待 + 重按 |
| 硬看门狗 | `SafetyStateMachine` | 800 ms 无合法应用控制 | 分级制动（0.3 → 0.6 → 最大）+ 撤销会话配置 |
| 恢复 | `SessionControlRecoveryLatch` | 所有条件连续健康 300 ms | 之后必须松键重按 |

要点：

1. 尖峰期间车处于 `DEGRADED` 滑行；全制动与重按发生在链路恢复之后。
2. ACK 判据用往返时间；单向命令完全新鲜时，也会因往返超过 200 ms 停车。
3. 生产环境只有 P2P 直连时才用 DataChannel 承载控制；中转时控制走 TCP 上的 WSS，
   丢包重传可能造成数百毫秒的队头阻塞。WSS 两端已开启 `TCP_NODELAY`。
4. 现有缺陷：进入制动等待（制动 1.0）后，如果链路继续中断到硬超时，
   `evaluate_control_watchdog` 改按分级值输出，制动回落到 0.3
   （`core.cpp:3743-3790`）。

## 3. 设计原则与不变量

1. 不放宽新鲜度，只改变短暂失去新鲜度之后的恢复方式。
2. 过期命令永远不驱动车辆：保持期间牵引恒为 0。
3. 保持有界：时长上限加频次预算，超出即升级为现有制动等待。
4. 故障类原因不进入保持，行为与现状相同。
5. 一次停车过程中制动只增不减。
6. 新行为需要车端和驾驶端双方能力协商，任一方不支持即保持现状。

## 4. 状态模型

| 阶段 | 进入条件 | 车辆输出 | 退出 |
|---|---|---|---|
| 正常 | — | 执行新鲜命令 | 出现瞬态原因进入保持；出现故障类原因进入制动等待 |
| 瞬态保持（新增） | 第 5.2 节的四类瞬态原因 | 挡位保持；转向保持在进入保持前最后一条新鲜命令的值；牵引 0；制动取驾驶员制动和 `hold_brake` 的较大值 | 满足恢复条件回到正常（不重按）；满足升级条件进入制动等待 |
| 制动等待（现有） | 保持升级，或故障类原因 | 全制动（1.0） | 现有规则：条件连续健康 300 ms 后松键重按 |
| 硬超时（现有） | `control_timeout_ms` 内无合法应用控制 | 分级制动，且不低于制动下限；撤销会话配置 | 现有规则 |

保持期间的"驾驶员制动"，取保持期间收到的最近一条新鲜命令中的制动值；没有新鲜命令时，
沿用进入保持前最后一条有效命令的制动值。转向和挡位不跟随保持期间收到的新命令。

默认参数下的时序：

- 控制断流：0–300 ms 正常；300 ms 进入保持（轻制动 0.15）；1000 ms 仍未恢复则升级
  为制动等待（1.0）；1200 ms 硬超时。
- 视频停顿：500 ms 进入保持；1200 ms 仍未恢复则升级为制动等待。视频保持期间命令
  照常到达，硬看门狗不会触发。
- 与现状对比（真断网）：现状 300–800 ms 滑行不加制动，800 ms 起 0.3，2300 ms 才到
  最大制动；新设计 300 ms 起 0.15，1000 ms 起 1.0。

## 5. 车端设计

### 5.1 组件

新增 `cpp/include/mine_teleop/detail/transient_hold.hpp`，定义 `TransientHoldPolicy`：
纯逻辑、无 I/O、自身无锁，由 `VehicleControlService` 持有，沿用该服务现有的串行化
（`control_mutex`）。职责：

- 维护当前活跃的瞬态原因集合和保持开始时间；
- 判断恢复（连续新鲜样本计数）与升级（时长、预算、故障、代际、时钟倒退）；
- 维护频次预算历史，有界，最多 `max_holds_per_window + 1` 条记录；
- 输出状态快照，供遥测使用。

接口要点（实现时可以调整命名）：

- `set_cause(cause, present, now_ms)`：某个瞬态原因出现或消失。
- `note_fresh_command(now_ms)`：收到一条新鲜、合法、各就绪标志为真的命令。
- `tick(now_ms)`：周期评估，返回 None / Hold / Resume / Escalate(reason)。
- `fault(now_ms)`、`epoch_changed(now_ms)`：保持期间出现故障类原因或代际变化，立即升级。
- `reset()`：新会话或会话配置变化时清空，包括预算历史。

### 5.2 瞬态原因（进入保持）

| 原因 | 判定 | 消失条件 |
|---|---|---|
| `control_gap` | 看门狗发现 `SafetyStateMachine` 进入 `DEGRADED`（距最后一条有效命令 ≥ `degraded_timeout_ms`） | 收到新鲜有效命令 |
| `late_commands` | 连续迟到达到会话配置的 `consecutive_late_frame_limit` | 收到新鲜有效命令 |
| `driver_video_stale` | 命令中 `critical_video_ready=false` 且 `critical_video_transient=true` | 命令中 `critical_video_ready=true` |
| `vehicle_video_stale` | 关键相机编码帧年龄超过 `critical_camera_stale_brake_ms`，或因过期、等待 IDR 而丢帧；相机未故障或停用，且没有 `invalid_sample_blocked` | 停顿后再出 3 个新鲜编码帧（沿用现有规则） |

车端视频的分类依据发送新鲜度的丢帧原因（`gstreamer_freshness.hpp`）：

- 过期（`Stale`）和等待 IDR（`WaitingIdr`）丢帧属于瞬态，包括由它们引起的连续异常计数和该路制动修订号变化。
- 非法时间戳或损坏访问单元（`invalid_sample_blocked`）、相机故障或停用属于故障。

协商生效后，喂给恢复闩锁的车端视频修订号只反映故障类事件。

### 5.3 恢复（不需要重按）

同时满足以下两点即恢复：

1. 没有任何活跃的瞬态原因；
2. 此后连续收到 `resume_fresh_commands`（默认 2）条新鲜有效命令，且各就绪标志均为真。

恢复后直接执行驾驶员当前的命令，牵引恢复速率由底盘桥现有的扭矩上升速率限制。
恢复时清除制动下限，并记录本次保持。

### 5.4 升级（转入现有制动等待）

满足任一条件即升级：

| 原因 | 条件 |
|---|---|
| `hold_duration_exceeded` | 保持持续 ≥ `max_hold_ms`（默认 700） |
| `budget_count_exceeded` | 本次保持会使窗口内次数超过 `max_holds_per_window`（默认 60 秒内 6 次） |
| `budget_time_exceeded` | 窗口内累计保持时长（含当前）超过 `max_hold_time_per_window_ms`（默认 3000） |
| `fault_during_hold` | 保持期间出现任一故障类原因 |
| `transport_epoch_changed` | 保持期间传输代际变化 |
| `clock_reversed` | 单调时钟倒退（安全失败） |

升级时调用现有的 `set_control_braking_wait(true, cause)`：全制动、要求重按，之后由
现有恢复闩锁接管。从升级到制动等待恢复之前，策略不再处理瞬态原因。

### 5.5 故障类原因（行为不变）

以下原因保持现有行为，直接进入制动等待或停车：

- 急停；
- 输入租约过期（`operator_input_ready=false`）；
- 校时未就绪；
- 驾驶端 `control_link_ready=false`；
- 驾驶端视频故障（`critical_video_ready=false` 且不是瞬态）；
- 车端相机故障、停用或非法样本；
- 驾驶端故障类制动修订号变化；
- 失去控制权或会话结束；
- 底盘、CAN、执行器故障；
- 非保持期间的传输代际变化。

### 5.6 制动下限（只增不减）

- 进入保持时，下限设为 `hold_brake`；进入制动等待时，下限设为 1.0。
- 硬超时的分级制动输出取分级值和下限中的较大值。下限只抬高制动值；急停锁存
  （`full_emergency_brake`）仍按分级值判断，锁存时机与现状相同。
- 保持正常恢复、制动等待恢复且驾驶员重新获得运动授权，或会话结束时，清除下限。
- 这同时修正第 2 节要点 4 的现有缺陷。

### 5.7 结果与遥测

- 保持期间收到的新鲜命令返回 `accepted=true`，附警告
  `transient_hold_traction_withheld`，不发出 `control_input_rearm_required`。
- 保持期间 `control_timing` 如实报告 `rearm_required=false`、
  `control_braking_wait=false`，并新增 `transient_hold` 对象：

```json
{"enabled": true, "active": true, "causes": ["control_gap"],
 "entered_at_monotonic_ms": 123456, "elapsed_ms": 180, "max_hold_ms": 700, "hold_brake": 0.15,
 "holds_in_window": 1, "hold_time_in_window_ms": 180, "budget_window_ms": 60000,
 "last": {"causes": ["late_commands"], "duration_ms": 240, "outcome": "resumed",
          "escalation_reason": null}}
```

- 运行日志事件：每次保持记 1–2 条，受频次预算约束：
  `vehicle_transient_hold_entered`（原因、断流时长或迟到条数、停顿的相机）、
  `vehicle_transient_hold_resumed`（持续时长）、
  `vehicle_transient_hold_escalated`（持续时长、升级原因）。
- `control_hard_stop_transition` 诊断附上最近一次保持的信息。

### 5.8 配置与校验

```yaml
control:
  rate_hz: 20
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 1200            # 开启瞬态保持的现场配置；代码默认仍为 800
  bridge_apply_timeout_ms: 800        # 底盘桥 apply 看门狗；缺省等于 control_timeout_ms
  transient_hold:
    enabled: true                     # 没有本块时视为 false，保持现有行为
    max_hold_ms: 700                  # 100–1500
    hold_brake: 0.15                  # 0–0.3
    resume_fresh_commands: 2          # 1–5
    budget_window_ms: 60000           # 10000–600000
    max_holds_per_window: 6           # 1–100
    max_hold_time_per_window_ms: 3000 # 100 至 budget_window_ms
```

- 启动校验：任何字段越界，或 `degraded_timeout_ms + max_hold_ms + 100 > control_timeout_ms`，
  都拒绝启动，错误信息指明具体字段。
- `control_timeout_ms` 原本同时传给车端状态机和底盘桥（`core.cpp:3060`、`core.cpp:3098`）。
  底盘桥用它做自己的 apply 看门狗：超时没有收到车端的成功 apply 就锁存急停，实际作用是
  检测车端进程卡死。为了不让这项检测随 1200 ms 一起变慢，新增
  `bridge_apply_timeout_ms` 专门传给底盘桥：取值 20–60000，不大于 `control_timeout_ms`，
  不小于 `field_safety.speed_feedback_timeout_ms`；缺省等于 `control_timeout_ms`（旧配置
  行为不变）。车端在保持、制动等待和 `DEGRADED` 期间每 50 ms 都会调用一次非锁存的 apply，
  所以 800 ms 的桥看门狗不会误触发。
- 仓库现场配置 `vehicle-agent.three-machine.field.yaml`、`vehicle-agent.ccg2-8m.yaml`
  显式开启，并把 `control_timeout_ms` 设为 1200；其他示例和台架配置不变。
- `redacted_summary` 输出生效的参数。

### 5.9 生效条件

仅当以下两点同时满足时生效：车端配置开启；当前会话最后一条合法命令带有签名字段
`transient_hold: "transient-hold-v1"`。断流期间沿用最后一条合法命令的协商结果。

### 5.10 集成点

| 位置 | 改动 |
|---|---|
| `VehicleControlService::receive`（`core.cpp:3472-3651`） | 迟到和断流后的首包不再直接进入制动等待，改为登记瞬态原因；驾驶端视频按瞬态和故障分流；保持期间不以 `degraded_neutral_required` 拒绝非中性命令，改为输出保持值，满足恢复条件后正常执行 |
| `VehicleControlService::tick`、`evaluate_control_watchdog`（`core.cpp:3701-3799`） | 进入 `DEGRADED` 时登记 `control_gap`；周期评估是否升级；保持期间每个看门狗周期施加保持输出；硬超时应用制动下限 |
| `set_control_braking_wait`（`core.cpp:3733`） | 制动下限设为 1.0；从保持进入时记录为升级 |
| `enforce_control_recovery`（`webrtc_media.cpp:3550-3620`） | 车端视频按 5.2 拆分为瞬态和故障；瞬态交给控制服务，不再直接让闩锁进入未就绪；驾驶端视频改用"故障"判定喂给闩锁 |
| offer 构造（`webrtc_media.cpp:1270` 附近） | 声明能力和容忍上限 |
| 配置加载（`core.cpp`） | 解析与校验 `transient_hold` |

## 6. 驾驶端设计

### 6.1 能力握手

- 驾驶端在 `server.cpp:7900-7911` 解析 offer。当
  `transient_hold_capability == "transient-hold-v1"`，且 `control_tolerance_ms` 是
  400–2000 之间的整数时，本次媒体协商视为支持，否则视为不支持，两端都退回现有行为。
  `video_tolerance_ms` 只用于界面显示，要求是 1–4500 之间的整数，不参与判据。
  每个新会话、新 offer 都重新判定。
- 支持时，在每条命令负载中加入签名字段 `transient_hold: "transient-hold-v1"` 和
  `critical_video_transient`，两者都在 MAC 覆盖范围内。

### 6.2 视频状态上报（页面）

- `criticalHealth` 的返回值新增 `transient`：当所有未就绪关键相机的原因都属于
  `presented_frame_stale`、`recovering_frames`、`awaiting_valid_frame`、`track_muted`，
  且修订号没有溢出时为真。
- 其余原因都判为故障：`camera_missing`、`track_missing`、`track_ended`、
  `video_paused`、`video_not_ready`、`no_presented_frame`、`peer_disconnected`、溢出。
- 意图新增可选布尔字段 `critical_video_transient`，缺省为 false，即按故障处理。
  原生运行时校验它只能和 `critical_video_ready=false` 同时为真，否则拒绝该意图。
- 协商生效后，瞬态类异常只递增 `bad` 修订号，连续 3 次时不再递增 `brake`；故障类事件
  （轨道身份或成员变化、溢出等）照旧递增 `brake`。未协商时不变。页面从原生状态
  `native_control.transient_hold.negotiated` 读取协商结果；读不到时按未协商处理。

### 6.3 ACK 判据与 DC 切换

`NativeControlAckRecovery` 新增容忍模式，容忍上限取车端声明的 `control_tolerance_ms`
（默认 1000）：

| 情况 | 现有规则 | 容忍模式 |
|---|---|---|
| 单次往返超过 200 ms | 连续 3 次进入等待 | 记为链路劣化（递增 `bad` 修订号），不进入等待 |
| 往返持续超过 200 ms | 3 次即进入等待 | 持续达到容忍上限才进入等待 |
| 有待确认包但 ACK 没有进展 | 500 ms 进入等待 | 达到容忍上限进入等待 |
| 进入等待后 | `control_link_ready=false`，递增制动修订号 | 不变（属于故障类） |

- `control_dc_ack_recovery_` 使用同一规则：只有 DC 断流，或往返持续异常达到容忍上限，
  才切到 WSS。
- 5 秒重连期限、128 个未确认包上限、恢复判据（新鲜 ACK）都不变。
- 已接受的取舍：回程持续高延迟（命令单向新鲜，但画面偏旧）的拦截时间，从约 150 ms
  放宽为持续 1000 ms。持续的高延迟仍然会停车。

### 6.4 原生状态

- `/api/status` 的 `native_control` 新增 `transient_hold` 对象，字段为 `negotiated`、
  `control_tolerance_ms`、`video_tolerance_ms`、`ack_rtt_anomaly_active`、
  `ack_rtt_anomaly_ms`、`ack_stall_ms`。
- 原生控制 trace 记录往返异常的开始和结束。

## 7. 页面

- **保持提示**：`controlRecoveryGuidance` 新增 `transient_hold` 阶段。文案为
  "链路/画面短暂波动，已暂停牵引（轻制动）· 无需操作，恢复后自动继续"，显示原因和
  相对 `max_hold_ms` 的进度，样式与制动等待区分开。
- **不清输入**：`applyVehicleSafetyState` 在车端处于保持（`transient_hold.active`）且
  `rearm_required=false` 时，不清除输入，也不提示重按。
- **升级后**：沿用现有的清除逻辑和提示；如果是频次预算触发的，附加
  "链路不稳定（1 分钟内保持 N 次）"。
- **画面层**：瞬态时显示"画面短暂中断"，故障沿用现有提示。
- **监控面板**：显示最近 1 分钟的保持次数、最近一次保持时长和升级次数。
- **日志**：页面事件日志新增 `driver_transient_hold_observed`（进入和退出各一条）；
  会话日志导出包含车端和页面的保持事件。

## 8. 协议与兼容

### 8.1 新增字段

| 载体 | 字段 | 说明 |
|---|---|---|
| 车端 offer | `transient_hold_capability` = `"transient-hold-v1"` | 配置开启时声明 |
| 车端 offer | `transient_hold` = `{control_tolerance_ms, video_tolerance_ms, max_hold_ms}` | 控制容忍 = `degraded_timeout_ms + max_hold_ms`；视频容忍 = `critical_camera_stale_brake_ms + max_hold_ms` |
| 驾驶端命令（签名） | `transient_hold` = `"transient-hold-v1"` | 仅在车端声明后发送 |
| 驾驶端命令（签名） | `critical_video_transient` | 布尔 |
| 页面 → 原生意图 | `critical_video_transient` | 布尔，可选 |
| 车端结果 | 警告 `transient_hold_traction_withheld` | 保持期间 |
| 车端遥测 | `control_timing.transient_hold` | 见 5.7 |

### 8.2 兼容矩阵

| 车端 | 驾驶端 | 行为 |
|---|---|---|
| 新版且开启 | 新版 | 瞬态保持 |
| 新版且开启 | 旧版 | 命令里没有能力标志，车端沿用现有行为 |
| 旧版 | 新版 | offer 里没有能力声明，驾驶端沿用现有 ACK 和视频规则 |
| 新版但 `enabled: false` | 任意 | 现有行为 |

### 8.3 云端

控制命令负载由云端整体透传（`server.cpp` 的 `enqueue_signaling_message`），并受端到端
MAC 保护。云端不需要更新，也不新增云端能力标志。

### 8.4 Windows 原生 mock 客户端

不发送能力标志，保持现有行为。

## 9. 测试与验证

### 9.1 单元测试（可移植 CTest，所有平台的 CI 都会跑）

- **保持策略组件：**
  - 窗口内恢复；
  - 700 ms 升级；
  - 60 秒内第 7 次保持升级；
  - 累计超过 3 秒升级；
  - 保持期间出现故障类原因或代际变化时升级；
  - 时钟倒退时直接升级；
  - 关闭时从不进入保持。
- **配置校验：**
  - 各参数范围；
  - `degraded_timeout_ms + max_hold_ms + 100 ≤ control_timeout_ms`；
  - 没有 `transient_hold` 块时视为关闭。
- **车端控制服务：**
  - 断流 250 ms 不保持。
  - 断流 500 ms 进入保持，并验证三项输出：牵引 0、转向停在最后新鲜值、制动取驾驶员制动和 0.15 的较大值。之后连续 2 条新鲜命令即恢复，不要求重按，非中性命令直接生效。
  - 断流 1100 ms 时，在第 1000 ms 由看门狗在没有任何报文的情况下升级为全制动 + 重按。
  - 3 条迟到命令进入保持。
  - 驾驶端视频瞬态进入保持；车端视频过期丢帧进入保持；非法样本直接进入制动等待。
  - 保持期间急停立即生效。
  - **制动只增不减的回归测试**：先进入制动等待，链路再断到硬超时，制动保持 1.0，不回落到 0.3。
  - 硬超时边界在 1199/1200 ms，撤销会话配置的行为不变。
  - 不带能力标志的命令完全沿用现有行为，现有测试全部保持通过。
- **驾驶端 ACK 判据：**
  - 短于 1000 ms 的尖峰不进入等待，也不递增制动修订号；
  - 往返持续超过 200 ms 满 1000 ms 时进入等待；断流满 1000 ms 时进入等待；
  - DC 只在持续异常时才切到 WSS；
  - 未协商时规则不变。
- **页面 JS：**
  - 视频原因的瞬态/故障分类；
  - 协商后，画面停顿不再递增制动修订号；
  - `DEGRADED` 且 `rearm_required=false` 时不清输入，并显示保持提示；
  - 升级路径不变。

### 9.2 集成测试

在 `control_transport_integration_tests` 上扩展，使用真实回环 WSS，加上驾驶端运行时和
车端控制服务：

- 在 WSS 路径上注入延迟：
  - 400 ms 尖峰：进入保持后自动恢复，不进入制动等待，驾驶端输入不被作废；
  - 1100 ms 尖峰：升级为制动等待 + 重按；
  - 60 秒内 7 次尖峰：按频次预算升级，显示"链路不稳定"。
- **回程单独延迟：** 驾驶端的 ACK 来自云端入口确认，回环夹具无法只拖慢这条回程，所以这项
  改在 ACK 判据的单元测试里覆盖（有待确认包但 ACK 停滞满 1000 ms、往返持续超过 200 ms 满
  1000 ms 都进入等待），并在车端服务测试里验证 `control_link_ready=false` 按故障类处理。
- **版本兼容：** 旧驾驶端（不发能力标志）时车端沿用现有行为；旧车端（不声明能力）时
  驾驶端沿用现有规则。

### 9.3 网络层尖峰注入（新增，只在 Linux 上运行）

- 在离线 CI 已有的隔离网络命名空间里（root 权限，只有回环），用 `tc netem` 给回环口
  注入真实的 300、600、1100 ms 延迟尖峰，跑一个端到端会话（信令服务 + 驾驶端运行时 +
  车端 mock 控制服务）。
- 这一步专门验证 TCP 重传、队头阻塞这类真实传输行为。
- 环境里没有 netem 时以返回码 77 标记为跳过，不会计为通过。

### 9.4 现场验收（由现场执行）

- **场景：** 直连（5G P2P）和中转（云端 WSS）各做一轮 30 分钟 mock 长测，再做一轮有人
  监护的实车低速测试。
- **分析脚本：** 新增 `scripts/test/transient_hold_analyze.py` 及其测试。输入导出的会话
  日志，统计保持次数和时长分布、升级次数及原因、硬超时次数。
- **验收标准：** 短于容忍上限的尖峰没有造成任何升级；每一次升级都能归因到超过上限的
  中断、频次预算用尽或故障类原因。
- **硬保护复核：**
  - 拔网线后 1000 ms 内全制动，1200 ms 硬超时；
  - 保持期间急停立即生效；
  - 拔掉相机立即进入制动等待。

### 9.5 每阶段的完成标准

- 本地用 Docker（Ubuntu 22.04）跑完整构建和全部 CTest，在 macOS 上跑可移植测试和
  离线 JS/Python 回归；最终以 CI 为准。
- 被跳过的安全测试不计为通过，在汇报中明确列出。

## 10. 部署、升级与回退

- 驾驶端和车端同提交更新；云端不需要更新，可以一并更新。
- 车端使用仓库现场配置即开启。自维护配置需要加上 `transient_hold` 块，并把
  `control_timeout_ms` 改为 1200；不满足约束的组合会被启动校验拒绝。
- 回退：车端 YAML 设 `enabled: false`、`control_timeout_ms: 800`，重启车端服务即可，
  不需要重新发包。
- `DEPLOY.md` 增加升级条目：`ice.*` 拒绝启动、硬看门狗与瞬态保持配置、回退步骤。
- 中转 WSS 的内核层调优（例如缩短最小重传超时）作为现场可选项写进部署文档，需要实测
  验证，代码默认不变。

## 11. 与 PR #26 修复阶段的合并顺序

全部在当前 PR 分支上进行，每个阶段单独提交，每阶段跑完 9.5 的验证再进入下一阶段。
推送前先征得同意。

1. **阶段 1：生产路径修复（A1–A7）。**
   - libnice 观测改为尽力而为；
   - 车端遇到 `ice.*` 仍拒绝启动，同时补齐升级文档；
   - keep-alive 测试注册进 CTest；
   - 网页和车端刷新 ICE 失败时降级而不是中止；
   - DC 试探重置补测试。
2. **阶段 2：精简 mock 双路实验。** 统一隔离 mock 判定（A6）放在本阶段删除之后执行，
   避免先统一一批随后就会删除的判定。
   - 删除：DC/WSS 镜像、紧凑 CBOR 控制和遥测格式、DC 上的媒体状态镜像，以及相关测试、
     构建目标和文档引用。
   - 保留并修复：B4（门控忽略旧副本）；B6（Windows 客户端必须看到车端证明才开启 mock
     控制，证明改为由车端配置推导：mock 适配器 + `test_bench_` + 0 km/h + 台架模式）；
     B7（接收端析构）。
   - 基于旧实现的 DC/WSS 对比证据，在文档中标注为失效。
3. **瞬态保持（本设计）。** 车端、驾驶端、页面三部分，加上第 9 节的测试和分析脚本。
4. **阶段 3：结构调整。**
   - 把只给台架和 viewer 用的头文件（包括 `native_receiver_core.hpp`）移到 `cpp/bench/`；
   - 补全 Windows 工作流的路径过滤；
   - 正式包里残留的台架代码用编译开关关掉；
   - 把 `run_mock_control` 拆成会话、输入、courier 循环、界面几个部分。
5. **阶段 4：流程整理（不拆 PR）。**
   - 重写 PR 说明；
   - 给测试重试加醒目标记和次数统计，并排查"正常场景触发硬看门狗"的根因：查到就修，
     查不到就记录结论；
   - 原始证据迁出仓库、只保留结论文档，迁移目的地另行确认；
   - 更新 `07-control-and-safety.md`、`08-configuration.md`、`DEPLOY.md`、
     `11-testing-and-validation.md`。

## 12. 风险与已知限制

- **转向保持：** 保持期间转向停在最后一条新鲜命令的值，最长 700 ms。这与现有
  `DEGRADED` 的转向处理一致。
- **回程高延迟：** 回程持续高延迟的拦截时间放宽到 1000 ms（见 6.3）。
- **零油门的实际效果：** 零油门在坡道上的物理效果由厂商底盘桥的速度 PID 决定，仓库内
  无法核实，所以保持期间施加轻制动。
- **超出上限仍会停车：** 超过容忍上限的中断仍然停车；频繁的瞬态会按频次预算升级，并
  提示链路不稳定。
- **共享最后一公里：** 直连和中转共享同一段接入网络，本设计不解决同一接入网络上的
  持续劣化。
- **验证边界：** 软件测试和回环注入不能替代现场与实车验收。

## 13. 决策记录（2026-11-04）

| 问题 | 决定 |
|---|---|
| 覆盖范围 | 控制链路 + 视频；不含输入租约和校时 |
| 保持期间行为 | 轻制动保持（默认 0.15，可配 0–0.3），恢复后自动继续 |
| 保持上限 | 700 ms；控制总容忍 1000 ms、视频总容忍 1200 ms；硬看门狗 800 → 1200 ms；车端 YAML 配置 + 启动校验 |
| 中转传输 | 维持现有选路，由瞬态保持兜底；内核层调优写进部署文档 |
| 方案 | 车端主导的瞬态保持 |
| 默认值 | 代码默认关闭；仓库现场配置开启 |
| 文档位置 | 本文件 |
| 规划阶段修订 | 新增 `bridge_apply_timeout_ms`（现场 800），保持底盘桥对车端进程卡死的检测时间不变；制动下限不改变急停锁存时机；回程单独延迟改为单元测试覆盖；A6 挪到阶段 2 |

## 14. 实施记录（2026-11-05）

实施按 `docs/superpowers/plans/2026-11-04-pr26-remediation-and-transient-hold.md` 的任务 12–22 逐个提交，每个提交都在
Ubuntu 22.04 容器里跑完整 CTest。与上文设计不同或设计没有写明的地方：

- **进入保持必须有真实断流。** 设计 5.2 写的是"进入 `DEGRADED`（距最后一条有效命令 ≥ `degraded_timeout_ms`）"，首版实现只检查了
  `DEGRADED`。但 `set_control_braking_wait(false)` 会经 `require_input_rearm()` 强制进入 `DEGRADED`，此时命令一直在到达。
  结果是每次制动等待恢复后都会产生一次假的 `control_gap` 保持。现在两个条件都要满足；该缺陷由真实回环集成测试发现。
- **视频分类不能改变未协商时的规则。** 车端逐路判定拆成 `effective_lane_anomalies` 和 `vehicle_video_lane_blocks_control`
  两个纯函数，并有穷举测试证明未协商时与原有布尔表达式逐位一致（连续 1–2 帧丢帧仍然容忍，第 3 帧才计）。
- **已认证的 ESTOP 不依赖保持字段。** 命令里未知或自相矛盾的 `transient_hold` 字段对 ESTOP 被忽略，对普通命令仍然拒绝。
- **ACK 容忍在每个周期里先于 ACK 消费设置**，对 WSS 和 DC 两个恢复对象同时生效，容忍为 0 时规则完全不变。
- **网络层测试用 100% 丢包，不是延迟。** 9.3 节写的是 netem 延迟尖峰；实现用 `loss 100%`（350 ms 与 1500 ms），因为丢包才会触发
  TCP 重传和队头阻塞——350 ms 的丢包使车端投递间隔延长到约 600 ms，仍应被保持吸收；测试同时断言丢包确实打断了投递，
  不会空过。需要 root 和 `sch_netem`，不满足时返回 77（CTest 记为跳过，不计为通过）。
- **集成测试夹具用真实单调时钟。** Colima VM 的挂钟约每 12 秒倒退约 100 ms，保持按设计看到时间倒退就升级（`clock_reversed`），
  所以夹具给车端服务注入 `steady_clock`。生产里车端本来就用 `steady_clock`。
- **页面文案：** 过期画面标签由"画面中断 / 暂无新帧"改为"画面短暂中断"。

## 15. 现场验收清单

1. 在车端导出 `vehicle-runtime.log`，用 `python3 scripts/test/transient_hold_analyze.py <日志文件...>` 汇总保持次数、时长分布
   （p50/p95/max）、升级原因和硬停车次数。
2. 验收标准同 9.4：短于容忍上限的尖峰没有任何升级；每次升级都能归因到超过上限的中断、频次预算用尽或故障类原因。
3. `escalation_reason` 的全部取值：`hold_duration_exceeded`（超过 `max_hold_ms`）、`budget_count_exceeded`、`budget_time_exceeded`、
   `clock_reversed`、`fault_during_hold`（保持期间出现故障类原因）、`transport_epoch_changed`（传输代际变化）、`hard_timeout`
   （硬看门狗接管）、`safety_latched`（故障/急停锁存接管）、`estop`（已认证的急停）。后三项表示更强的停车接管了保持，不是保持本身的失败。
4. 事件名：`vehicle_transient_hold_entered` / `vehicle_transient_hold_resumed` / `vehicle_transient_hold_escalated`
   （见 `docs/24-vehicle-runtime-diagnostics.md`），驾驶端页面日志里对应 `driver_transient_hold_observed`。
