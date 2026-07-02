# 控制协议与安全停车

## 设计目标

控制链路必须简单、稳定、可追溯。视频可以丢帧，控制不能积压旧命令。

如果控制命令使用 WebRTC DataChannel，通道必须配置为 unordered/unreliable。控制命令是 20 Hz 全量状态，车端依赖 `seq` 丢弃旧命令；可靠有序重传会造成队头阻塞，不适合作为默认控制模式。

## ControlCommand

建议字段：

```json
{
  "type": "control_command",
  "protocol_version": 1,
  "vehicle_id": "vehicle-001",
  "session_id": "session-001",
  "seq": 12345,
  "ts_ms": 1780000000000,
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
- `session_id`：当前控制会话 ID，必须是 JSON string。
- `seq`：单调递增的非负 JSON integer，用于丢弃乱序旧命令。
- `ts_ms`：驾驶端生成时间，必须是 JSON integer，用于审计、日志对齐和延迟估算；安全判定不能直接依赖两端系统时钟差，明显偏斜时只记录 warning。
- `gear`：档位，必须是 JSON string，具体枚举待车辆接口确认。
- `steering`：归一化转向，必须是 JSON number，范围 `[-1.0, 1.0]`。
- `throttle`：归一化油门，必须是 JSON number，范围 `[0.0, 1.0]`。
- `brake`：归一化刹车，必须是 JSON number，范围 `[0.0, 1.0]`。
- `estop`：急停，必须是 JSON boolean，不能用 `"true"`/`"false"` 字符串。
- `authority_token`：可选控制权令牌；如果出现，必须是 JSON string。

控制命令中的 JSON string、number、integer 和 boolean 字段不能互相用字符串、
布尔值或数字代替。

## 发送频率

默认 20 Hz。

原则：

- 固定周期发送完整状态。
- 没有输入变化也要发送心跳式命令。
- 车端以最后一条有效命令的本地接收时间判断链路健康。

## 车端校验

车端收到命令后：

1. 校验消息格式。
2. 校验 `protocol_version` 是否兼容。
3. 校验 session_id。
4. 校验控制权。
5. 校验 seq 是否大于已处理序号。
6. 使用本地接收时间检查命令到达间隔是否超过配置阈值。
7. 校验驾驶端时间戳是否明显异常，并记录到日志；除非有可靠时间同步，不直接用跨机器时间差拒绝控制。
8. 校验控制值范围。
9. 如果 `estop=true`，立即锁存进入急停状态。
10. 将命令交给安全状态机。

## 时间同步

系统必须有最低限度的时间同步要求：

- 车端、驾驶端和云端至少启用 NTP，启动时记录同步状态和当前偏差估计。
- `ts_ms` 可用于审计、录像元数据对齐、延迟估算和多系统日志排障。
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
  "session_id": "session-001",
  "ts_ms": 1780000000100,
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
