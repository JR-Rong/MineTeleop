# 控制协议与安全停车

## 设计目标

控制链路必须简单、稳定、可追溯。视频可以丢帧，控制不能积压旧命令。

控制命令只通过 WebRTC DataChannel 传输，信令服务拒绝 `control_command`。通道固定为
`label=control`、`protocol=mine-teleop-control-v1`、`ordered=false`、
`maxRetransmits=0`。控制命令是 20 Hz 全量状态，车端依赖 `seq` 丢弃旧命令；
可靠有序重传会造成队头阻塞，不适合作为控制模式。

## ControlCommand

建议字段：

```json
{
  "type": "control_command",
  "protocol_version": 1,
  "vehicle_id": "vehicle-001",
  "driver_id": "driver-001",
  "session_id": "session-001",
  "seq": 12345,
  "sent_at_utc_ms": 1780000000000,
  "control_token": "short-lived-session-token",
  "gear": "D",
  "steering": 0.12,
  "throttle": 0.20,
  "brake": 0.00,
  "estop": false
}
```

字段说明：

- `protocol_version`：控制协议版本，必须是 JSON integer，用于车端/驾驶端兼容性检查。
- `vehicle_id`：目标车辆 ID，必须是 JSON string。
- `driver_id`：当前获权驾驶员 ID，必须是 JSON string，并与会话记录一致。
- `session_id`：当前控制会话 ID，必须是 JSON string。
- `seq`：单调递增的非负 JSON integer，用于丢弃乱序旧命令。
- `sent_at_utc_ms`：控制输入形成时的 UTC 毫秒时间，必须是 JSON integer；两端时间同步不确定度必须不超过 25 ms，否则时延数据标记为不可信。
- `control_token`：当前会话的短期控制权令牌，必须是非空 JSON string；认证续租只延长
  其服务端到期时间，不在活动 DataChannel 中轮换值；会话结束后立即失效，禁止写入日志。
- `gear`：档位，必须是 JSON string，具体枚举待车辆接口确认。
- `steering`：归一化转向，必须是 JSON number，范围 `[-1.0, 1.0]`。
- `throttle`：归一化油门，必须是 JSON number，范围 `[0.0, 1.0]`。
- `brake`：v1 线协议中的归一化普通行车制动，必须是 JSON number，范围
  `[0.0, 1.0]`。它本身不是 EHB bar 或制动力 N；车端只在已经确认的当前会话控制
  参数内，将它换算成明确的 EHB 压力请求。驾驶端的缓刹和急刹都以 bar 配置，再映射
  到这一线协议标量。
- `estop`：急停，必须是 JSON boolean，不能用 `"true"`/`"false"` 字符串。

控制命令中的 JSON string、number、integer 和 boolean 字段不能互相用字符串、
布尔值或数字代替。

## 发送频率

默认 20 Hz。

原则：

- 固定周期发送完整状态。
- 没有输入变化也要发送心跳式命令。
- 浏览器只允许一个 async writer 串行执行 `/api/control` 和 DataChannel `send`；键盘、Gamepad 和
  心跳只更新 latest-wins 快照，不排队积压过时命令。
- 车端对 `vehicle_telemetry` 与 `vcu_handshake_status` 共用单调递增的 `control_status_seq`；浏览器在
  unordered DataChannel 上只接受严格递增状态，禁止旧 Ready 覆盖较新的 fault/disarm。只有同时通过 `control_status_seq` 门禁并且通过
  `command_seq` 关联到当前换挡事务的拒绝才能改变本地挡位。
- D/R 选择与油门按键状态分离；松开前进/倒车键只归零牵引请求，普通制动也不自动切 N。
  真实断链、控制权丢失和 VCU 故障/退出仍重置控制资格，并要求新的 keydown 才能恢复。
- 浏览器失焦或页面隐藏时立即清空物理输入，保持已选 D/R，但发送零牵引、零转向、零普通制动的
  安全快照；旧按键不能在窗口恢复焦点后自动恢复控制。
- DataChannel 未打开、关闭或缓冲超过上限时不继续生成有效油门，界面显示控制链路中断/拥塞。
- 车端以最后一条有效命令的本地接收时间判断链路健康。

## 车端校验

车端收到命令后：

1. 校验消息格式。
2. 校验 `protocol_version` 是否兼容。
3. 校验 `vehicle_id`、`driver_id` 和 `session_id` 均与当前会话一致。
4. 校验当前 `control_token`；空令牌、旧会话令牌和其他会话令牌均拒绝。
5. 校验 seq 是否大于已处理序号。
6. 使用本地接收时间检查命令到达间隔是否超过配置阈值。
7. 校验驾驶端时间戳是否明显异常，并记录到日志；除非有可靠时间同步，不直接用跨机器时间差拒绝控制。
8. 校验控制值范围。
9. 如果 `estop=true`，立即锁存进入急停状态。
10. 将命令交给安全状态机。

## 会话控制参数

普通驾驶开始前，驾驶端必须先通过同一条 DataChannel 发送
`type=session_control_profile`。当前 `profile_version=3` 是完整快照，包含目标车速、
单电机最大转矩、普通制动最大/缓刹/急刹压力、最大转角、车速 PID 的
`kp/ki/kd/derivative_filter_tau_ms/max_dt_ms`，以及电机升扭斜率
`motor_torque_rise_rate_nm_per_s`。车端只有在 bridge 原子应用整份快照后才发送
`event=session_control_profile_status` 的成功状态；未确认参数时拒绝 VCU 握手和非
急停驾驶命令。参数在断链、控制权/会话替换、故障或 adapter 自有安全停车时清除，
重连后不能沿用旧确认；急停始终绕过普通参数门禁。成功状态的 `applied_revision`
必须等于请求 `seq`，拒绝时为 0，浏览器不能用不同 revision 的 ACK 解锁控制。

非 mock 车端会把会话参数限制在车端 YAML 的不可绕过上限内：目标车速不超过
`max_speed_kph * max_throttle`，单电机转矩不超过
`full_scale_motor_torque_nm`，三项普通制动压力满足
`service <= hard <= max <= max_brake_pressure_bar`，转角不超过
`max_steering_angle_deg`，PID 仍受车端绝对范围约束。首次设置、提高目标车速/转矩、
修改任一制动压力/转角/PID/升扭斜率还要求新鲜的 N 挡、零速和 EPB 驻车反馈，且 bridge
必须处于 `standby` 或 `disarmed`；Ready 中不能热改这些参数。会话清除会撤销牵引、
复位 PID 并恢复车端 YAML 的默认 PID 与默认升扭斜率。反馈超时、硬超速 margin、命令超时和降速曲线
始终是车端只读安全参数，控制端不能覆盖。

## 车端本地车速闭环与换挡门禁

真实底盘适配器把 `throttle` 解释为已确认会话目标车速内的比例，车端目标不超过
该会话的 `target_speed_kph`，同时仍受 `field_safety.max_speed_kph` 和
`field_safety.max_throttle` 约束。API 线程只保存最新意图；
bridge 的单一 SocketCAN I/O 线程每 20 ms 用新鲜的带符号 VCU 车速运行 PID，并在
同一线程串行调用 ChassisControl 计算转向和制动。PID 输出范围是 `[0, 1]`，直接乘以
已确认的会话单电机转矩上限，并以相同幅值写入八路牵引通道；不再把 PID 输出解释为
理想加速度后通过整车质量、轮径和减速比二次换算。车端
`field_safety.motor_torque_rise_rate_nm_per_s` 给出加扭斜率默认标定，会话 V3 起
控制端面板可在 `[0, 32000] Nm/s` 包络内按会话调整该斜率（修改共享 PID 驻车门槛）；
`0` 表示关闭额外斜率，严格按 PID 输出直接换算。启用正值时，当周期可达转矩会作为 PID 的动态输出
上限，因此条件积分能感知斜率限制，不会在外层限幅器后继续积累；任何减扭仍立即生效。
到达目标点时积分项可以保留维持车速所需的正扭矩；每个电机通道的唯一牵引上限是
已确认会话上限与 `full_scale_motor_torque_nm` 的较小值，`max_throttle` 不是额外
扭矩系数。PID 以固定目标参考
应用 0.05 m/s 复位死区：手柄小抖动不丢积分，累计偏移越过死区或目标明显下降才复位。

DBC 中八路 `ADU_Tx_MCUxxMotTqReq` 都是 0.1 Nm 分辨率，物理范围
`[-800, 838.3] Nm`。普通驾驶采用 D/R 对称代码上限
`min(800 * 0.8, 838.3 * 0.8)`，并按 0.1 Nm 向零量化为
`640.0 Nm/单电机`：反向恰为 DBC 下界绝对值的 80%，正向约为上界的 76.34%。
车端默认上限为 `300 Nm/单电机`，会话只能继续收紧。
这些值是 CAN 转矩请求上限，不是实测电机或轮端转矩。

DBC 中 EHB01/EHB02 的八路压力请求都是 12-bit、0.1 bar 分辨率，物理范围
`0..409.5 bar`。普通驾驶代码上限取 80%，即 `327.6 bar/路`；车端默认普通最大
压力为 `100 bar/路`，控制端默认普通最大/缓刹/急刹分别为
`100/30/100 bar/路`。本 PR 的缓刹和急刹都是直接压力请求：缓刹不运行独立制动
PID，也没有 ramp/jerk 曲线；急刹直接请求会话中配置的急刹压力。

制动优先于牵引：任何正制动都把油门和目标车速置零、复位车速 PID、将八路电机
扭矩置零，并在保留转向请求的同时向八路 EHB 写入 0.1 bar 量化后的直接压力。零油门、非 Ready、换挡、实际挡位不匹配、车速
反馈无效/过期或异常控制周期也复位 PID 并归零牵引。未经台架验证的 VCU 车速请求
不参与闭环，`ADU_Tx_VehSpdReq` 在所有状态固定为 `0 km/h / Q=0`。

进入 Ready 后，bridge 对任何挡位变更（包括切入 N）都要求新鲜有效的挡位/车速反馈，
且绝对车速不大于 0.1 m/s；拒绝换挡时先撤销旧牵引并保留上一有效挡位。换挡闭环期间保持转角和 EHB
压力连续，但八路扭矩保持为零。`WaitParkingBrakeReleased`、`WaitGear` 和
`WaitActuatorModes` 都要求静止，车速超过 0.1 m/s 立即锁存停车并转入反向退出；
每个首次启动等待态还有 500 ms 分层反馈宽限，超时后 CAN 静默不能无限保持 EPB
释放。曾到达 Ready 后，apply 和 29 路关键反馈 watchdog 在后续换挡等待态继续生效。
Ready 阶段即使零牵引或制动，车速超过配置最大值与独立 margin，或运动方向与所选
D/R 相反超过 0.1 m/s，仍会锁存本地安全停车；普通控制命令不能解除。ESTOP 在
WaitParallel 阶段撤销握手并保持 EPB 驻车，在之后的启动阶段立即进入带 EHB 安全
制动的完整退出。恢复必须
完成退出流程，并在新鲜反馈满足 N、零速、EPB 驻车、人工状态后显式重新握手。

Bridge 拒绝未停稳或反馈过期时的任何换挡请求时，现有牵引意图先在车端撤销。拒绝结果通过同一次 ABI 调用返回
枚举原因，vehicle runtime 再用 `control_command_rejected` 状态事件向当前 DataChannel
发送 allowlist 中的稳定 `issue_code`。`vcu_drive_gear_change_moving_or_stale` 会让控制页
清空油门/制动、按 `command_seq` 恢复拒绝前挡位，立即发送该挡位零牵引，并要求驾驶员释放方向键后重新选择；不会因拒绝自动回 N。原始异常字符串不会发送给
浏览器。无法关联的拒绝会冻结普通控制（急停和显式断开仍可用），直到安全断开并重新
握手。bridge 还会锁存被拒绝的换挡：旧目标挡在之后降到零速时也不会自动生效；只有
上一有效挡位的零牵引或制动命令、显式断开，或新握手才能解除。锁存期间旧挡上的普通
制动保持可用。相同原因最多每 500 ms 重发一次，以兼顾 unordered/unreliable 通道丢包与限频。

急停、物理急停、故障、断开停车和 bridge 本地 apply watchdog 走独立安全路径：
八路牵引归零并可请求 DBC 全量 `409.5 bar/路`，不受会话的 327.6 bar 代码上限或
车端 100 bar 普通默认值削弱。上游控制心跳超时仍按配置的 0.3/0.6/1.0 分段执行，
Degraded 阶段会先把会话归一化制动换算成车端普通压力比例，因此保持原物理压力；
即使普通压力比例为 1.0，也不会被误判为全量急停。最终超时 1.0 阶段才通过显式
安全标志使用 409.5 bar。仓库 field 配置中的 PID、转矩和压力值只是软件
默认/门禁值，不是台架或实车验收值；软件构建、单测和虚拟 CAN 通过也不等于实车
制动或车速闭环已验收。
`deceleration_profile` 首段必须从 `after_ms: 0` 开始、时间严格递增、制动比例不下降，
并最终达到显式 1.0；否则车端配置加载直接失败，避免超时策略永远缺少最终全量安全制动。

## 时间同步

系统必须有最低限度的时间同步要求：

- 车端和驾驶端启动后对服务器 `/time` 进行 7 次四时间戳采样，选取低 RTT 样本估算偏移、RTT 和不确定度，并定期刷新。
- `sent_at_utc_ms` 可用于审计、录像元数据对齐、控制有效接收时延和多系统日志排障。
- 不确定度超过 25 ms 时，车端不得进入远程控制，控制页面必须显示“时延数据不可信”。
- 控制安全的新鲜度判定以车端本地接收时间、`seq` 和心跳间隔为准。
- 如果后续需要多相机严格同步、事故复盘级时间线或更高精度闭环，再评估 PTP 或相机硬件同步。

## 安全停车

默认策略：

- 控制心跳短暂异常先进入降级控制：油门置 0、限制速度、提示驾驶端链路抖动。
- 超过 `control_timeout_ms` 后进入 `TIMEOUT_BRAKE`。
- 普通驾驶的缓刹/急刹按会话中已确认的 bar 值直接施加；本 PR 不实现制动 PID 或
  ramp。控制心跳超时按配置的 0.3/0.6/1.0 分段执行，故障、断链、急停以及最终
  1.0 阶段使用独立安全停车路径，不能被普通会话上限削弱。
- 车辆未停稳前不默认挂 N；是否保持当前驱动档、进入低速档或切换安全档位必须结合车型制动语义确认。
- 车辆停稳后，才执行驻车/手刹/安全档位等停稳后动作。
- 维持安全停车直到重新建立有效会话并完成复位流程，或由现场人员复位。

配置示例：

```yaml
control:
  rate_hz: 20 # 固定的上游命令频率（50 ms）；bridge CAN I/O 独立固定为 20 ms/50 Hz
  freshness_mode: local_receive_interval_and_seq
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  timeout_action:
    deceleration_profile:
      - after_ms: 0
        brake: 0.3
      - after_ms: 500
        brake: 0.6
      - after_ms: 1500
        brake: vehicle_defined_max_safe
```

参数语义：

- `rate_hz`：当前只支持固定 `20 Hz` 上游命令频率；其他值在配置加载时 fail closed。
  这不是 bridge 的 `20 ms` SocketCAN 发送/PID 周期。
- `max_command_gap_ms`：单次有效命令到达间隔上限。超过该值时，车端应丢弃过旧命令、记录链路异常，并可提示驾驶端网络抖动；它不是状态机进入降级态的持续时间。
- `degraded_timeout_ms`：链路异常持续时间阈值。超过该持续时间后进入降级控制，例如油门置 0、限速或告警；本 PR 不在普通制动路径内生成压力 ramp。
- `control_timeout_ms`：持续未收到有效控制心跳后进入 `TIMEOUT_BRAKE` 的阈值。该值必须小于按车辆制动距离、安全边界和场地速度上限反推得到的最大允许值。

`degraded_timeout_ms=300` 只能作为首版弱网告警/降级参考值，不应直接等同于急刹阈值。5G 抖动可能达到几十到上百毫秒，最终 `max_command_gap_ms`、`degraded_timeout_ms`、`control_timeout_ms` 和安全制动动作必须结合真实网络、车辆制动距离、坡道/松散路面和底层控制器心跳机制实测标定。
三个毫秒参数都必须为正且不大于 60000，并满足
`degraded_timeout_ms < control_timeout_ms`。`deceleration_profile.after_ms` 从进入
`TIMEOUT_BRAKE` 起算，沿用非负、声明顺序严格递增和最终 1.0 的语义。

## 急停

急停优先级最高。

要求：

- 驾驶端可触发。
- 车端本地可触发。
- 车辆底层如有独立急停，必须优先使用。
- 车端收到一次 `estop=true` 即锁存进入 `ESTOP`，不依赖驾驶端持续发包。
- 急停进入后，控制输出必须进入车辆定义的急停/安全停车策略。
- 急停解除必须走显式复位流程，不应只靠驾驶端按钮。
- 本地参考实现中，车端控制服务只接受带本地确认和授权人的复位调用，复位成功后写入 `estop_reset` 审计事件。
- 真实车辆接入前必须定义谁有权解除、是否必须现场物理确认、是否需要双人确认、如何记录审计日志。

## Telemetry

建议字段：

```json
{
  "type": "telemetry",
  "protocol_version": 1,
  "vehicle_id": "vehicle-001",
  "driver_id": "driver-001",
  "session_id": "session-001",
  "seq": 12346,
  "sent_at_utc_ms": 1780000000100,
  "speed_mps": 2.5,
  "gear": "D",
  "steering_feedback": 0.10,
  "throttle_feedback": 0.18,
  "brake_feedback": 0.00,
  "safety_state": "CONTROL_ACTIVE",
  "fault_flags": [],
  "link": {
    "control_rtt_ms": 60,
    "video_rtt_ms": 70,
    "packet_loss": 0.01
  }
}
```

## Vehicle Adapter

车辆底层接口未确认，所以使用适配器。

首版：

- `MockVehicleAdapter`
- 只记录命令。
- 模拟 Telemetry。

后续：

- `CanVehicleAdapter`
- `DynamicLibraryVehicleAdapter` 的本地 C shim 路径已实现，目标车辆主机仍需联调验证。

真实车辆接入前必须补充：

- 控制量单位和范围。
- 档位枚举。
- 刹车控制语义。
- 底层控制器心跳。
- 底层安全停车能力。
- 命令确认或状态反馈方式。
