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
- `brake`：归一化刹车，必须是 JSON number，范围 `[0.0, 1.0]`。
- `estop`：急停，必须是 JSON boolean，不能用 `"true"`/`"false"` 字符串。

控制命令中的 JSON string、number、integer 和 boolean 字段不能互相用字符串、
布尔值或数字代替。

## 发送频率

默认 20 Hz。

原则：

- 固定周期发送完整状态。
- 没有输入变化也要发送心跳式命令。
- 浏览器失焦或页面隐藏时立即清空输入并发送中性状态。
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
