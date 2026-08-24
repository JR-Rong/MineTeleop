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
- `brake`：归一化普通行车制动，必须是 JSON number，范围 `[0.0, 1.0]`。驾驶端的缓刹和
  急刹是两个可配置请求值，但在 v1 中仍共用这一标量；该值不是 EHB bar 或制动力 N。
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
  unordered DataChannel 上只接受严格递增状态，禁止旧 Ready 覆盖较新的 fault/disarm。
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

## 车端本地车速闭环与换挡门禁

真实底盘适配器把 `throttle` 解释为目标车速比例，车端目标为
`clamp(throttle, 0, 1) * field_safety.max_speed_kph`。API 线程只保存最新意图；
bridge 的单一 SocketCAN I/O 线程每 20 ms 用新鲜的带符号 VCU 车速运行 PID，并在
同一线程串行调用 ChassisControl。PID 输出范围是 `[0, 1]`，到达目标点时积分项
可以保留维持车速所需的正扭矩；每个电机通道的唯一牵引上限是
`full_scale_motor_torque_nm`，`max_throttle` 不是额外扭矩系数。PID 以固定目标参考
应用 0.05 m/s 复位死区：手柄小抖动不丢积分，累计偏移越过死区或目标明显下降才复位。

制动优先于牵引：任何正制动都复位 PID、将八路电机扭矩置零，同时继续通过
ChassisControl 生成 EHB 压力。零油门、非 Ready、换挡、实际挡位不匹配、车速
反馈无效/过期或异常控制周期也复位 PID 并归零牵引。未经台架验证的 VCU 车速请求
不参与闭环，`ADU_Tx_VehSpdReq` 在所有状态固定为 `0 km/h / Q=0`。

车端和 VCU 状态机都要求进入 D/R 或 D/R 互换时具有新鲜有效的挡位/车速反馈，且
绝对车速不大于 0.1 m/s；拒绝换挡时先撤销旧牵引。换挡闭环期间保持转角和 EHB
压力连续，但八路扭矩保持为零。`WaitParkingBrakeReleased`、`WaitGear` 和
`WaitActuatorModes` 都要求静止，车速超过 0.1 m/s 立即锁存停车并转入反向退出；
每个首次启动等待态还有 500 ms 分层反馈宽限，超时后 CAN 静默不能无限保持 EPB
释放。曾到达 Ready 后，apply 和 29 路关键反馈 watchdog 在后续换挡等待态继续生效。
Ready 阶段即使零牵引或制动，车速超过配置最大值与独立 margin，或运动方向与所选
D/R 相反超过 0.1 m/s，仍会锁存本地安全停车；普通控制命令不能解除。ESTOP 在
WaitParallel 阶段撤销握手并保持 EPB 驻车，在之后的启动阶段立即进入带 EHB 安全
制动的完整退出。恢复必须
完成退出流程，并在新鲜反馈满足 N、零速、EPB 驻车、人工状态后显式重新握手。

升级时必须重新标定 `full_scale_motor_torque_nm`。旧实现的有效上限曾是
`full_scale_motor_torque_nm * max_throttle`；新版 PID 可以使用完整 full-scale，
直接复用旧值可能把请求放大到原来的十倍。仓库 field 配置中的 PID 参数只是通过
schema 范围校验的占位值，不是台架或实车验收值。

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
- 刹车按分级减速曲线渐进施加，不默认阶跃到 `brake=1.0`。
- 车辆未停稳前不默认挂 N；是否保持当前驱动档、进入低速档或切换安全档位必须结合车型制动语义确认。
- 车辆停稳后，才执行驻车/手刹/安全档位等停稳后动作。
- 维持安全停车直到重新建立有效会话并完成复位流程，或由现场人员复位。

配置示例：

```yaml
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
```

参数语义：

- `max_command_gap_ms`：单次有效命令到达间隔上限。超过该值时，车端应丢弃过旧命令、记录链路异常，并可提示驾驶端网络抖动；它不是状态机进入降级态的持续时间。
- `degraded_timeout_ms`：链路异常持续时间阈值。超过该持续时间后进入降级控制，例如油门置 0、限速、告警或按配置开始柔和减速。
- `control_timeout_ms`：持续未收到有效控制心跳后进入 `TIMEOUT_BRAKE` 的阈值。该值必须小于按车辆制动距离、安全边界和场地速度上限反推得到的最大允许值。

`degraded_timeout_ms=300` 只能作为首版弱网告警/降级参考值，不应直接等同于急刹阈值。5G 抖动可能达到几十到上百毫秒，最终 `max_command_gap_ms`、`degraded_timeout_ms`、`control_timeout_ms` 和制动曲线必须结合真实网络、车辆制动距离、坡道/松散路面和底层控制器心跳机制实测标定。

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
