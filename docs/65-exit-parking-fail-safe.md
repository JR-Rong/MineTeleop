# 65 退出会话与安全退出的驻车闭锁

状态：**已实现，待实车/台架复验**。基线：`rjr/cx/native-realtime-pr26`
的 `6b020b9`。本文只处理退出路径上的软件驻车闭锁，不替代物理急停或独立硬件
安全回路。

## 1. 现象与影响

现场反馈：关闭页面/结束会话（“退出会话”）或点击“安全退出”后，车辆没有自动
拉起电子驻车，停在坡道时会溜车。安全退出的页面流程本身会发送 VCU 断开请求，
车端也会在会话结束时关闭控制适配器；问题不在“是否发起退出”，而在退出过程中
的制动输出顺序。

## 2. 退出链路

- “安全退出”：页面经控制通道发送 VCU `disconnect` → 车端桥
  `request_park()` → `ParallelController::request_disarm()` 进入反向握手：
  零扭矩 → 零速 → N → EPB 驻车 → 人工状态 3。
- “退出会话/关闭页面”：控制 DataChannel 关闭或云端会话结束 → 车端
  `VehicleControlService::close()` → `VehicleAdapter::close()` → 桥 `close()`，
  同样执行上述反向握手，并等待 `disarmed` 与末批 CAN 发送成功。

两条路径最终都依赖同一个反向握手；因此驻车缺陷也相同。

## 3. 缺陷

### 3.1 反向握手先写 EPB 释放

`DisarmTorque / DisarmStop / DisarmNeutral` 仍位于“武装阶段释放 EPB”的分支内。
退出可以从已经驻车的状态开始；反向握手再次写入 EPB=1（释放），如果后续任一阶段
拿不到新鲜反馈而停在超时，末批请求就可能是“释放”，车辆不带驻车退出。

### 3.2 驻车建立期间丢掉 EHB 行车制动

进入 `DisarmParkingBrake` 时只写 EPB=2，未写 EHB 的 by-wire 模式与压力。此前
`DisarmNeutral` 的 EHB 全制动在这一拍被撤掉，车辆在 N 挡上只等电子驻车建立。
驻车机构有建立时间，坡道上就出现可溜车窗口；若驻车反馈始终不来，窗口持续到
15 秒退出超时。

### 3.3 服务制动失败会跳过整段退出

`VehicleControlService::close()` 先调用 `apply_safe_stop()`，再调用
`adapter_->close()`。前者抛出时（例如桥已拒绝普通 apply、传输故障或适配器
状态不允许），后者不会执行：反向握手、EPB 驻车和末批确认全部被跳过。

## 4. 修复

1. **只在武装阶段释放 EPB。** `WaitParkingBrakeReleased / WaitGear /
   WaitActuatorModes / Ready` 保留释放请求；反向握手各阶段只写“保持”(0)，
   不再写释放。
2. **驻车确认前保持 EHB。** `DisarmParkingBrake` 与 `DisarmManual` 写
   EHB by-wire 模式与校准急停压力；只有进入 `Disarmed`（四路 EPB 新鲜反馈均为
   2）后才撤销 EHB，并在末批继续保留 EPB=2、N 请求和低握手。
3. **退出必须执行关闭。** `VehicleControlService::close()` 捕获服务制动异常，
   仍然调用 `adapter_->close()` 完成反向握手与驻车确认；随后再抛出首个错误，
   保留原有诊断语义。二次 close 不会重复执行 15 秒退出。
4. 驻车确认失败时（VCU 不回报 N/零速/四路 EPB=2），末批不再出现 EPB 释放；
   软件仍报告 `disarm_timeout / safe_exit_unconfirmed`，要求现场使用独立硬件
   安全路径处置。

## 5. 回归

- `mine-teleop-vcu-tests`：新增 `stalled_exit_keeps_service_brake_and_never_releases_epb`，
  覆盖零扭矩、车辆仍在移动、N 挡未确认、驻车反馈缺失各阶段，断言每拍都有校准
  EHB 且从不写 EPB=1；扩展原反向握手用例，断言驻车阶段 EHB 保持、确认驻车后
  EHB 才撤销。
- 新增 `mine-teleop-close-fail-safe-tests`：服务制动抛错时仍执行适配器关闭；
  适配器关闭错误只执行一次并向调用方传播。
- 本机 macOS/arm64 完整构建后运行 CTest；Linux 专属的
  `mine-teleop-chassis-bridge-diagnostics-smoke` 中的安全退出/末批用例仍需在
  Linux CI 或台架执行。

## 6. 现场复验

1. 在安全坡道上完成一次“安全退出”，采集车端 VCU CAN 日志，确认从反向握手
   开始到驻车确认期间 EHB 一直为 by-wire 且有压力；末批为四路 EPB=2、N、
   低握手；确认车辆保持静止后再释放页面。
2. 关闭页面/结束会话重复一次，确认同样进入反向握手且不出现 EPB=1 末批。
3. 受控台架制造“驻车反馈缺失”，确认 15 秒内 EHB 持续、页面提示
   `safe_exit_unconfirmed`，车辆不因软件释放驻车而移动。
4. 复验后仍需供应方确认 EPB 执行器实际夹紧与 VCU 对末批帧的接受语义；CAN
   日志中的 EPB=2 不能单独证明机械驻车力。

## 7. 残留风险

- 进程被 SIGKILL、断电或 CAN 物理失效时，软件无法执行反向握手；必须依赖
  物理急停/独立硬件驻车。
- VCU 不回报零速或 N 时反向握手会超时；本次修复保证超时过程中不主动释放
  驻车、不撤销 EHB，但不代替硬件安全回路。
- 自动驻车（`parking_idle_timeout_ms`）与本修复互补：前者处理驾驶空闲，
  后者保证退出路径闭锁；两者都不改变物理执行器能力。
