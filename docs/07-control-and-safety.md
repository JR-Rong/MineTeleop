# 控制协议与安全停车

## 设计目标

控制链路必须简单、稳定、可追溯。视频可以丢帧，控制不能积压旧命令。

## ControlCommand

建议字段：

```json
{
  "type": "control_command",
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

- `seq`：单调递增，用于丢弃乱序旧命令。
- `ts_ms`：驾驶端生成时间。
- `gear`：档位，具体枚举待车辆接口确认。
- `steering`：归一化转向，范围 `[-1.0, 1.0]`。
- `throttle`：归一化油门，范围 `[0.0, 1.0]`。
- `brake`：归一化刹车，范围 `[0.0, 1.0]`。
- `estop`：急停。

## 发送频率

默认 20 Hz。

原则：

- 固定周期发送完整状态。
- 没有输入变化也要发送心跳式命令。
- 车端以最后一条有效命令时间判断链路健康。

## 车端校验

车端收到命令后：

1. 校验消息格式。
2. 校验 session_id。
3. 校验控制权。
4. 校验 seq 是否大于已处理序号。
5. 校验时间戳是否过旧。
6. 校验控制值范围。
7. 如果 `estop=true`，立即进入急停状态。
8. 将命令交给安全状态机。

## 安全停车

默认策略：

- 控制心跳超时后进入 `TIMEOUT_BRAKE`。
- 油门置 0。
- 刹车按配置施加。
- 档位置为安全档位。
- 维持安全停车直到重新建立有效会话或现场复位。

配置示例：

```yaml
control:
  rate_hz: 20
  command_max_age_ms: 200
  control_timeout_ms: 300
  timeout_action:
    throttle: 0.0
    brake: 1.0
    gear: N
```

`control_timeout_ms=300` 是首版建议默认值，需要结合车辆制动特性、网络抖动和底层控制器心跳机制实测调整。

## 急停

急停优先级最高。

要求：

- 驾驶端可触发。
- 车端本地可触发。
- 车辆底层如有独立急停，必须优先使用。
- 急停进入后需要明确复位流程。

## Telemetry

建议字段：

```json
{
  "type": "telemetry",
  "vehicle_id": "vehicle-001",
  "session_id": "session-001",
  "ts_ms": 1780000000100,
  "speed_mps": 2.5,
  "gear": "D",
  "steering_feedback": 0.10,
  "throttle_feedback": 0.18,
  "brake_feedback": 0.00,
  "safety_state": "ACTIVE",
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
- `DynamicLibraryVehicleAdapter`

真实车辆接入前必须补充：

- 控制量单位和范围。
- 档位枚举。
- 刹车控制语义。
- 底层控制器心跳。
- 底层安全停车能力。
- 命令确认或状态反馈方式。

