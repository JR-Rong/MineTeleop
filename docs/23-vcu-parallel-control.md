# JYR010 VCU 平行驾驶通讯与实车验收

## 实现与证据边界

本仓库按 `JYR010_DBC_VCU_20260714.dbc` 和
`JYR010_通讯协议_VCU_20260714.xls` 实现车端 VCU 通讯。当前环境无法连接真实
CAN/VCU，因此已经完成的是协议编解码、状态机、20 ms 调度和日志实现；没有完成
实车握手、执行器响应、制动效果或总线负载验收。

这里的 `20 ms` 是 bridge SocketCAN I/O/PID 的固定 50 Hz 周期；上游
`control.rate_hz` 独立固定为 `20 Hz`（50 ms），loader 会拒绝其他值。

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
| Body/VehSpd | `0x18FDD0F5`, `0x18FED0F5` | 2 | Body 保留；VehSpd 永久发送 `0 km/h / Q=0` |

DBC 对八路 MCU 转矩请求的定义相同：0.1 Nm 分辨率，范围
`[-800, 838.3] Nm`。普通驾驶采用 D/R 对称代码上限
`min(800 * 0.8, 838.3 * 0.8)`，按 0.1 Nm 向零量化后为
`640.0 Nm/单电机`；反向恰为 DBC 下界绝对值的 80%，正向约为上界的 76.34%。
车端和控制端默认上限均为 `300 Nm/单电机`。

EHB01 控制制动 1--4，EHB02 控制制动 5--8；每路压力请求都是 12-bit、0.1 bar
分辨率、范围 `0..409.5 bar`。普通驾驶代码上限取 80%，即 `327.6 bar/路`；默认
普通最大/缓刹/急刹为 `100/30/100 bar/路`。急停、物理急停、故障、断开停车、
bridge 本地 apply watchdog 以及上游控制超时的最终 1.0 阶段是独立安全路径，可请求
DBC 全量 `409.5 bar/路`；上游控制超时在此之前仍按 0.3/0.6 普通压力分段执行。
这些数值都是 CAN 请求，不代表实测电机转矩、管路压力或整车制动力。

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
4. 发送 N/R/D 挡位和 EPS/EHB 模式，等待挡位反馈；进入驱动挡或 D/R 互换都要求
   新鲜有效车速不大于 0.1 m/s。
5. 发送八路 MCU 扭矩模式，等待 MCU/EPS/EHB 全部模式反馈为 1。
6. 只有完成以上反馈闭环后才发送本地 PID 计算出的扭矩、ChassisControl 转角和
   会话中已确认的直接 EHB 压力。

电机八路始终使用 `MotCtrlMode=1` 扭矩模式，不会在扭矩模式和电机转速模式之间往返切换。
本实现不启用未经台架验证的 VCU 车速闭环；无论 Ready、制动、换挡或故障，
`ADU_Tx_VehSpdReq` 都固定编码为 `0 km/h / Q=0`。

车速控制在车端 bridge 的唯一 SocketCAN I/O 线程执行。PID 启动默认值来自车端 YAML；
`profile_version=2` 可在安全驻车门禁内由控制端按会话提交五个 PID 参数。API 线程只保存最新控制意图；
I/O 线程每个 20 ms 周期用带符号的 VCU 车速反馈运行一次 PID，再串行调用
ChassisControl，避免 vendor 接口并发。油门是已确认会话目标车速内的比例，最终
目标同时受 `field_safety.max_speed_kph * max_throttle` 约束。任何正油门只表示启用 PID，
不是第二个扭矩上限；PID 输出固定截断到 `[0, 1]`，到达目标点后允许积分项保留
克服滚阻或坡度所需的正扭矩。D 使用正向速度和正扭矩，R 使用反向速度和负扭矩，
PID 输出直接乘以会话单电机转矩上限后以相同幅值写入八路牵引通道，不再经过
`m*a`、轮径和减速比的理想车辆模型换算。最终每路仍按会话转矩上限与
`full_scale_motor_torque_nm` 的较小值、挡位方向和 0.1 Nm 向零量化硬限幅。
`motor_torque_rise_rate_nm_per_s=0` 表示不增加额外斜率；配置正值时，当周期可达
转矩直接作为 PID 的动态 ceiling，条件积分能感知限制。任何减扭立即生效。
ChassisControl 继续负责转向和制动计算。
PID 目标参考采用固定 0.05 m/s 复位死区；参考不随每次小抖动移动，因此累计偏差
越界时仍会复位，目标明显下降或 D/R 改变也会复位。

普通制动、零油门、非 Ready、换挡、实际挡位不匹配、车速无效/过期、异常 PID
周期都会清空 PID 并输出零牵引。V4 继承 V3 的物理压力制动输入：bridge 仍调用
ChassisControl 保持转向计算，但随后把八路电机扭矩强制置零，并用 0.1 bar 量化后的
会话压力直接覆盖八路 EHB 请求。本 PR 的缓刹是直接 `service_brake_pressure_bar`，
没有独立制动 PID 或 ramp/jerk 曲线；急刹直接使用 `hard_brake_pressure_bar`。
`WaitGear` 和 `WaitActuatorModes` 阶段保持最新转角和 EHB 请求连续，但八路扭矩始终为零。

`WaitParkingBrakeReleased`、`WaitGear` 和 `WaitActuatorModes` 是静止收敛阶段；新鲜
车速绝对值超过 0.1 m/s 就锁存停车，不能等到最大车速加 margin。首次启动进入每个
阶段后有 500 ms 宽限，随后按阶段检查必要反馈：WaitParking 检查握手、选择器、
车速和 EPB，WaitGear 另加实际挡位，WaitActuatorModes 检查全部关键反馈。这样最后
一帧 EPB 释放反馈后 CAN 静默不会无限保持释放请求。

Ready 阶段的独立硬超速门限使用 `max_speed_kph + hard_overspeed_margin_kph`，
在零牵引或制动时也持续监测，不随瞬时油门目标降低；实际运动方向与 D/R 期望
相反且超过 0.1 m/s 也锁存安全停车。普通 apply
不能清除此锁存。只有完成完整退出，且新鲜反馈再次满足 N、绝对车速不大于
0.1 m/s、四路 EPB 驻车和人工状态 3 后，显式重新请求握手才会清除。

ESTOP 不允许启动状态机继续正向推进：在 `WaitParallelHandshake` 撤销 ShakeReq 并
维持 EPB 驻车；从 `WaitParkingBrakeReleased` 起则立即进入 `DisarmTorque`，下一拍
启用 EHB 安全制动并按零扭矩、零速、N、EPB 驻车、人工状态的完整顺序退出。
WVCU 物理急停开关在 VehicleStatus 接收时立即锁存，即使开关脉冲在同一个 20 ms
周期内恢复也不会重新使用旧牵引命令；普通 apply 或软急停清除不能解除。只有开关
已释放、完整退出到 Disarmed，且新鲜反馈再次满足 N、零速、EPB 驻车和人工状态后，
显式重新请求握手才清除该锁存。首次尚未取得控制权的 Standby 场景也必须通过相同
驻车门禁显式请求握手。

安全退出顺序：

1. 扭矩请求归零并等待八路扭矩反馈都在 ±2 Nm 内。
2. 八路 EHB 请求 DBC 全量 `409.5 bar/路`，等待车速不高于 0.1 m/s。
3. 请求 N 并等待 N 反馈。
4. 请求四路 EPB 驻车值 2，并等待四路状态都为 2。
5. 清除 `ShakeReq`，等待人工状态 3。

控制端通过同一条双向 DataChannel 每 500 ms 接收
`vcu_handshake_status`，可见 N/R/D 选择器、车速、EPB、VCU 状态、当前状态机阶段
和最终 `ready`。
只有 `ready=true` 才放行普通驾驶命令。点击“断开 VCU 握手”、DataChannel
断开或安全退出时，车端执行上述完整反向序列；不会只停发 CAN。

控制页面的“实车调试限幅”窗口可设置当前浏览器会话的目标车速、单电机最大转矩、
普通制动最大压力、缓刹压力、急刹压力和最大四轴转向角。控制端默认分别为
`2 km/h`、`300 Nm/路`、`100/30/100 bar/路` 和 `3°`；`Space` 是可释放的缓刹，
`B` 是可释放的急刹，`E` 仍是锁存安全急停。两档行车制动在 v1 线上仍通过
`brake` 归一化标量传输，但车端只在已经确认的会话最大压力内还原为明确 bar 值。
车端对同时出现的油门和制动采用制动优先，任何正制动请求都会撤销牵引。ChassisControl
更新失败或输出 NaN/Inf 时，bridge 立即锁存本地安全停车，不等待上游控制超时。
键盘与 Gamepad 共用会话参数；修改参数会先清零当前输入。真实 adapter 的首次设置、
提高目标车速/转矩或修改任一制动压力/转角/PID 都要求新鲜的 N 挡、零速和 EPB
驻车反馈，且 controller 必须为 Standby 或 Disarmed。
车端仅在实际应用参数后确认；未确认参数时不能请求 VCU 握手或发送普通驾驶命令。
参数会在断链、故障、控制权/会话替换和 adapter 安全停车时清除；bridge 同时撤销牵引、
复位 PID 并恢复当前 open 配置中的 YAML 默认 PID。成功 ACK 的 `applied_revision` 必须等于请求 `seq`。
车端 `field_safety.max_throttle`（目标车速比例上限）、
`field_safety.full_scale_motor_torque_nm`、
`field_safety.max_brake_pressure_bar` 和
`field_safety.max_steering_angle_deg` 是不可由浏览器绕过的第二层硬上限，车端会
通过 DataChannel 把实际硬上限回传给窗口。
`field_safety.motor_torque_rise_rate_nm_per_s` 是车端启动配置，不进入会话参数；
实际值记录在有效配置日志和 bridge 的 `vehicle_parameters` 事件中。压力上限必须来自车辆标定；
急停、故障、断开停车和 bridge 本地 apply watchdog 走独立安全停车路径；上游控制
超时的最终 1.0 阶段同样不受普通驾驶刹车限幅削弱，之前的 0.3/0.6 阶段仍按车端
普通压力上限换算。Degraded 保持上一条普通制动的物理压力：会话比例先换算为车端
普通压力比例；普通比例 1.0 与全量 409.5 bar 安全制动由显式标志区分，不再依赖
浮点数值猜测语义。
超时分段首项必须为 `after_ms: 0`、时间严格递增、制动比例不下降并最终包含 1.0；
重复时间、缺少最终全量阶段或中途降低制动的配置都会 fail closed。
非 mock 车端升级后必须在 YAML 中显式补齐
`field_safety.max_brake_pressure_bar` 和
`field_safety.motor_torque_rise_rate_nm_per_s`；旧的归一化 `max_brake` 会被拒绝。

首次到达 Ready 后，会按 `control.control_timeout_ms` 监视成功 apply 的新鲜度，且
该会话标志在后续 `WaitGear`/`WaitActuatorModes` 不清除；到期时将八路扭矩置零、
施加独立的 `409.5 bar/路` 安全制动，并只记录一次 `vcu_control_apply_timeout`。Ready 内下一条有效
apply 可清除软锁存；换挡等待态超时会进入完整退出，不能靠普通 apply 继续启动。
它与下述 CAN feedback 新鲜度 watchdog 相互独立。

进入 Ready 前必须收到八路扭矩反馈。Ready 后会分别监视握手、VCU 状态、车速、
物理挡位选择器、EPB、八路 MCU 模式、八路 MCU 扭矩、四轴 EPS 和四组 EHB 共
29 个关键反馈 ID；这个 watchdog 在曾 Ready 后的换挡等待态继续保持，直到退出或
新握手重置；
任一 ID 超过 500 ms 未更新，bridge 都会记录具体 `stale_ids`、锁存通讯故障并
发送零扭矩和安全制动控制。这个软件动作不能替代 VCU 自身的报文超时保护，后者必须
在实车验收中独立确认。
其中本地 PID 对车速反馈使用更短、独立配置的
`field_safety.speed_feedback_timeout_ms`；过期时当周期清 PID 并归零牵引，不等
500 ms 全量 watchdog。

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
  full_scale_motor_torque_nm: 300.0
  # 0 表示关闭额外升扭斜率；正值必须经隔离台架标定。
  motor_torque_rise_rate_nm_per_s: 0.0
  # 以下 PID 值必须由隔离台架标定，不能直接照抄示例上车。
  speed_feedback_timeout_ms: 200
  speed_pid_kp: 1.0
  speed_pid_ki: 0.2
  speed_pid_kd: 0.0
  speed_pid_derivative_filter_tau_ms: 100.0
  speed_pid_max_dt_ms: 100
  hard_overspeed_margin_kph: 3.6
  max_brake_pressure_bar: 100.0
  max_steering_angle_deg: 5.0
  require_can_feedback_before_control: true

vehicle_adapter:
  type: can
  integration:
    chassis_control:
      can_interface: can1
      bridge_library_path: /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so
```

`full_scale_motor_torque_nm` 是本地 PID 可使用的车端单电机通道上限，允许
`0..640.0 Nm`，默认 `300 Nm`，`0` 禁用驱动力；当前会话上限只能比它更低。
`max_throttle` 只限制会话可选目标车速。`max_brake_pressure_bar` 允许
`0..327.6 bar/路`，默认 `100 bar/路`，也只能被会话继续收紧。未完成隔离 CAN
台架标定前不得切换真实 adapter；默认值不是实车验收值。

`max_speed_kph` 不大于当前 ChassisControl 接口上限 `72 km/h`，用于计算本地 PID
目标及独立硬超速基准，不再受未使用的 VCU 1 km/h 请求分辨率约束。这个软件 PID
和限幅不是已验证的 VCU/底盘硬件限速器。修改后先运行 `config-check`、`--preflight` 和
`--adapter-status`，确认 bridge、`libchassis_control.so`、`can1` 和日志目录都
可用，再连接驾驶员。不得通过把 `require_can_feedback_before_control` 改成
`false` 绕过反馈门禁。

vehicle-agent runtime 与 bridge 必须原子成套升级。当前 runtime 要求 bridge 提供
ABI version 4、完全一致的 V4 配置结构大小、兼容 V3/V2 大小查询以及
`mine_teleop_chassis_open_v4`；
同时要求同次返回结构化拒绝结果的 `mine_teleop_chassis_apply_state_v2`、runtime-control
V1 大小查询，以及原子 configure/clear 符号。任一大小或必需符号不匹配都会在
任何 SocketCAN 初始化前被拒绝。`apply_state_v2` 只返回固定枚举，不传递日志字符串；
其中 D/R 移动或反馈过期可由 runtime 安全映射成稳定 issue code，未知值统一降级为
通用拒绝。旧 `apply_state` 继续保留整数 fail-closed 包装。只提供 V1 的旧 bridge 会明确启动失败，
V2 bridge 也会因缺少物理普通制动压力上限而失败，不会静默沿用旧的负加速度
语义。新 bridge 仍为直接 ABI 调用方和兼容性测试导出
`mine_teleop_chassis_open_v1`、`mine_teleop_chassis_open_v2` 与
`mine_teleop_chassis_open_v3`：V1 因为没有安全 PID 配置而只允许零牵引，V2 保留
负值表示减速度的旧语义，V3 没有斜率字段，因此直接 PID 转矩不增加额外升扭斜率；这些入口不能使旧
vehicle-agent 通过全局 ABI version 4 启动门禁。WebRTC 视频链与这一
控制故障隔离：视频先协商并显示，控制 DataChannel 打开后才尝试启动 adapter；
adapter 启动或运行失败会短暂上报握手 `fault`、关闭控制 DataChannel、拒绝
驾驶命令并继续视频；关闭控制通道同时保护旧版本控制端不误报远程急停。该隔离不
绕过配置校验，缺少实车必填 `field_safety` 键时进程仍会在媒体启动前拒绝运行。

```bash
/opt/mine-teleop/bin/mine-teleop-run config-check \
  --config /opt/mine-teleop/config/vehicle-agent.yaml \
  --chassis-bridge-library /opt/mine-teleop/lib/vendor/chassis/libmine_teleop_chassis_bridge.so
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
6. 从重新标定后的极小 `full_scale_motor_torque_nm` 开始，小幅验证四轴正负转向、
   D/R 八路小扭矩和八路小压力，逐项核对物理方向、单位、符号、比例、饱和和反馈。
   特别核对轻油门只选择较低目标车速、PID 在目标点可保持正扭矩，以及制动时八路
   扭矩为零但 EHB 压力连续。
7. 断开驾驶控制但保持 CAN，确认超时制动；再中断 VCU 反馈，确认 500 ms 通讯
   故障、日志和 VCU 自身超时策略。
8. 在安全可控的低速条件验证硬超速和反向运动检测，仅能通过完整退出、零速驻车
   门禁和显式重新握手恢复；普通控制 apply 不得恢复牵引。
9. 执行正常关闭，确认扭矩归零、N、EPB=2、人工状态 3 的完整退出结果。
10. 保存日志、`ip -details -statistics link show can1`、VCU 版本、DBC/XLS 版本、
   车辆载荷、轮胎状态和现场视频，作为验收记录。

任何一步反馈值、方向或执行结果不一致，都应停止后续阶段，保留日志并按具体 CAN
ID、cycle 和状态迁移定位，不能通过放宽门禁继续测试。
