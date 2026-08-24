# JYR010 VCU 平行驾驶通讯与实车验收

## 实现与证据边界

本仓库按 `JYR010_DBC_VCU_20260714.dbc` 和
`JYR010_通讯协议_VCU_20260714.xls` 实现车端 VCU 通讯。当前环境无法连接真实
CAN/VCU，因此已经完成的是协议编解码、状态机、20 ms 调度和日志实现；没有完成
实车握手、执行器响应、制动效果或总线负载验收。

实现入口：

- `cpp/include/mine_teleop/vcu.hpp`
- `cpp/src/vcu.cpp`
- `deployments/chassis-control-bridge/chassis_control_bridge.cpp`
- `cpp/tests/vcu_tests.cpp`

MinePilot ControlUI 用于核对 SocketCAN 接收、反馈报文和实际操作顺序；
ChassisControl 用于核对八轮控制量、启动/退出状态机。平行驾驶功能按 VCU 联调
要求复用两者已经验证的智能驾驶握手步骤 `ShakeReq=2 -> status=5`，但仍使用
本仓库基于 20260714 协议实现的 CAN 编解码和 EPB 枚举，不能照搬旧代码中不一致
的 EPB 反馈值。

## 报文与状态机

所有 ADU 发送报文都是 29-bit 扩展帧、8 字节、20 ms 周期：

| 类别 | CAN ID | 数量 | 主要内容 |
| --- | --- | ---: | --- |
| MCU01-08 | `0x18F0D0F5`-`0x18F7D0F5` | 8 | 使能、扭矩模式、扭矩、转速 |
| EPS01/03 | `0x18F8D0F5`, `0x18F9D0F5` | 2 | 四轴模式、角度、角速度 |
| EHB01/02 | `0x18FFD0F5`, `0x18FAD0F5` | 2 | 八路模式、制动压力 |
| EPB | `0x18FBD0F5` | 1 | 四路保持/释放/驻车、转向模式 |
| Shake | `0x18FCD0F5` | 1 | 挡位、智驾握手、故障复位 |
| Body/VehSpd | `0x18FDD0F5`, `0x18FED0F5` | 2 | 当前保留为无请求 |

车端启动 CAN bridge 后只进入 `standby`，继续发送 20 ms 的低请求报文，但不会
自动声明平行驾驶控制权。开始握手同时要求：

1. 驾驶员的 WebRTC `control` DataChannel 已连接；
2. 驾驶员在控制端点击“开始平行驾驶握手”；
3. `WVCU_GearCtrlReqSts=1`（物理选择器为 N）；
4. VCU 车速反馈有效且绝对值不大于 0.1 m/s；
5. 四路 EPB 转发状态均为 2（驻车）；
6. `WVCU_ShakeHandSts=3`（VCU 处于人工状态）；
7. 上述门槛反馈均在最近 500 ms 内收到。

这里使用 `WVCU_GearCtrlReqSts` 判断驾驶员的物理选择器是否为 N。实车只有
N/R/D 三个挡位；电子驻车通过四路 EPB 状态单独判断，不能用不存在的 P 挡代替。

启动顺序：

1. 满足上述门槛并收到显式开始命令后，连续发送 5 个周期的低握手和受限控制量。
2. 复用智驾握手：发送 `ShakeReq=2`、保持 `CloudShakeReq=0`，等待
   `WVCU_ShakeHandSts=5`。
3. 发送 EPB 释放请求 1，等待四路转发状态都为 1。
4. 发送 N/R/D 挡位和 EPS/EHB 模式，等待挡位反馈。
5. 发送八路 MCU 扭矩模式，等待 MCU/EPS/EHB 全部模式反馈为 1。
6. 只有完成以上反馈闭环后才发送扭矩、转角和制动压力。

安全退出顺序：

1. 扭矩请求归零并等待八路扭矩反馈都在 ±2 Nm 内。
2. 施加由 ChassisControl 的 -8 m/s² 参数计算出的制动压力，等待车速不高于
   0.1 m/s。
3. 请求 N 并等待 N 反馈。
4. 请求四路 EPB 驻车值 2，并等待四路状态都为 2。
5. 清除 `ShakeReq`，等待人工状态 3。

控制端通过同一条双向 DataChannel 每 500 ms 接收
`vcu_handshake_status`，可见 N/R/D 选择器、车速、EPB、VCU 状态、当前状态机阶段
和最终 `ready`。
只有 `ready=true` 才放行普通驾驶命令。点击“断开 VCU 握手”、DataChannel
断开或安全退出时，车端执行上述完整反向序列；不会只停发 CAN。

控制页面的“实车调试限幅”窗口可设置当前浏览器会话的最大油门/纵向输入、最大
普通驾驶刹车和最大四轴转向角。默认分别为 5%、100% 和 3°，键盘与 Gamepad
共用这一限幅；修改限幅会先清零当前输入，而且每次应用都要求确认车辆处于隔离
台架。车端 `field_safety.max_throttle`、`field_safety.max_brake` 和
`field_safety.max_steering_angle_deg` 是不可由浏览器绕过的第二层硬上限，车端会
通过 DataChannel 把实际硬上限回传给窗口。归一化刹车上限必须来自车辆标定；
急停、控制超时、故障和断开停车走独立安全停车路径，不受普通驾驶刹车限幅削弱。
非 mock 车端升级后必须在 YAML 中显式补齐 `field_safety.max_brake`；未完成标定时
先显式使用 `1.0`，不要臆造现场制动值。

进入 Ready 前必须收到八路扭矩反馈。Ready 后会分别监视握手、VCU 状态、车速、
物理挡位选择器、EPB、八路 MCU 模式、八路 MCU 扭矩、四轴 EPS 和四组 EHB 共
29 个关键反馈 ID；
任一 ID 超过 500 ms 未更新，bridge 都会记录具体 `stale_ids`、锁存通讯故障并
发送零扭矩和安全制动控制。这个软件动作不能替代 VCU 自身的报文超时保护，后者必须
在实车验收中独立确认。

## 日志落盘

真实车辆运行前必须提供可写目录。默认使用：

```bash
export MINE_TELEOP_VCU_LOG_PATH=/var/log/mine-teleop/vcu-can.jsonl
export MINE_TELEOP_VCU_LOG_MAX_BYTES=134217728
export MINE_TELEOP_VCU_LOG_ROTATIONS=10
```

日志是 JSONL。关键 `kind`/`name`：

- `can_tx_batch`：每个 20 ms 周期完整的 16 帧、状态、`send_ok` 和失败 ID；
- `can_rx`：每个被协议层识别的反馈帧；
- `event/control_parameters`：控制物理量快照；
- `event/feedback_snapshot`：握手、EPB、挡位、模式、扭矩、转角、压力和车速；
- `event/state_transition`：状态迁移；
- `event/parallel_handshake_requested`、`parallel_handshake_rejected`：
  开始请求及 N/零速/EPB/人工状态/反馈新鲜度门槛；
- `event/parallel_handshake_disconnect_requested`：控制端主动断开请求；
- `event/emergency_stop`、`feedback_timeout`、`can_send_failed`、
  `can_receive_failed`、`tx_deadline_miss`、`disarm_complete`、
  `disarm_timeout`：安全、调度与故障结果。

安全/状态事件立即 flush，普通帧至少每秒 flush。文件达到上限后轮转为 `.1` 到
`.N`；排查时需要同时保存当前文件和全部轮转文件。

## 从 mock 切换到真实 CAN bridge

仓库现场模板和安装包默认使用 `vehicle_adapter.type: mock`，这是为了避免安装后
意外给底盘上电。实车调试时修改车端实际使用的
`/opt/mine-teleop/config/vehicle-agent.yaml`（源码模板是
`configs/vehicle-agent.three-machine.field.yaml`），不要修改控制端配置来切换
adapter。至少需要：

```yaml
hardware:
  can:
    interface: can1
    bitrate: 500000
    tx_queue_length: 100

field_safety:
  commissioning_mode: bench
  max_speed_kph: 5
  max_throttle: 0.10
  full_scale_motor_torque_nm: 41.25
  max_brake: 1.0
  max_steering_angle_deg: 5.0
  require_can_feedback_before_control: true

vehicle_adapter:
  type: can
  integration:
    chassis_control:
      can_interface: can1
      bridge_library_path: /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so
```

`full_scale_motor_torque_nm` 表示直行、制动为 0、稳态、有效 `throttle=1.0`
时的单电机通道目标值，允许 `0..165 Nm`，`0` 禁用驱动力。实际稳态上限仍会
乘以 `max_throttle`，而且受 ChassisControl `300 Nm/s` 斜率限制，不能把它理解
为瞬间跳变值或实测轮端转矩。`max_speed_kph` 对非 mock adapter 必须大于 0；
当前扭矩模式下它不是可替代 VCU 限速器的硬件速度闭环。修改后先运行
`config-check`、`--preflight` 和
`--adapter-status`，确认 bridge、`libchassis_control.so`、`can1` 和日志目录都
可用，再连接驾驶员。不得通过把 `require_can_feedback_before_control` 改成
`false` 绕过反馈门禁。

runtime 与 bridge 必须成套升级。当前 runtime 要求 bridge 提供
`mine_teleop_chassis_open_v1`；旧 bridge 会在任何 CAN 初始化前明确启动失败，
不会继续控制车辆并静默忽略 `full_scale_motor_torque_nm`。WebRTC 视频链与这一
控制故障隔离：视频先协商并显示，控制 DataChannel 打开后才尝试启动 adapter；
adapter 启动或运行失败会短暂上报握手 `fault`、关闭控制 DataChannel、拒绝
驾驶命令并继续视频；关闭控制通道同时保护旧版本控制端不误报远程急停。该隔离不
绕过配置校验，缺少实车必填 `field_safety` 键时进程仍会在媒体启动前拒绝运行。

```bash
/opt/mine-teleop/bin/mine-teleop-run config-check \
  --config /opt/mine-teleop/config/vehicle-agent.yaml
/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml --preflight
/opt/mine-teleop/bin/mine-teleop-run vehicle-agent \
  --config /opt/mine-teleop/config/vehicle-agent.yaml --adapter-status
```

## 未来实车验收清单

必须在车轮离地、动力隔离或同等风险控制的台架上执行，不能直接道路验证。

1. 配置 `can1` 为 500 kbit/s、`txqueuelen 100`，确认 error-active、无 bus-off、
   无持续错误计数；运行时还会核对接口、波特率并设置发送队列。
2. 仅启动 bridge，不给驾驶命令；核对 16 个 TX ID、扩展帧标志、8 字节 DLC、
   20 ms 周期和总线负载。
3. 逐阶段核对日志中的 `state_transition` 与 VCU 反馈：
   `initial -> wait_parallel_handshake -> wait_parking_brake_released ->
   wait_gear -> wait_actuator_modes -> ready`。
4. 在每一阶段人为缺失对应反馈，确认控制量不会越过当前门禁。
5. 分别验证 N/R/D；只有物理选择器为 N 且电子驻车已拉起时才允许开始握手，
   D/R 均应被准入门禁拒绝。
6. 小幅验证四轴正负转向、八路小扭矩和八路小压力，逐项核对物理方向、单位、符号、
   比例、饱和和反馈。
7. 断开驾驶控制但保持 CAN，确认超时制动；再中断 VCU 反馈，确认 500 ms 通讯
   故障、日志和 VCU 自身超时策略。
8. 执行正常关闭，确认扭矩归零、零速、N、EPB=2、人工状态 3 的完整退出结果。
9. 保存日志、`ip -details -statistics link show can1`、VCU 版本、DBC/XLS 版本、
   车辆载荷、轮胎状态和现场视频，作为验收记录。

任何一步反馈值、方向或执行结果不一致，都应停止后续阶段，保留日志并按具体 CAN
ID、cycle 和状态迁移定位，不能通过放宽门禁继续测试。
