# 安全退出驻车与持续前进叠加转向诊断

基线：`main` 的 `fb271156b117779402a9b945a3982c457432c248`。
证据来自用户提供的 `session-000020(1).zip` 和 `session-000021.zip`，
下列时间均为日志中的 UTC。两个 manifest 都标记 `complete=false`，CAN 文件有
`size_or_scan_limit`，因此不能把导出包当作完整现场录像或物理制动验收。

## 安全退出后的驻车请求

`session-000020` 的 `vehicle/vcu-can.jsonl`：

| 时间 | 证据 |
| --- | --- |
| 02:17:44.495 | `session_lost`，开始完整反向退出 |
| 02:17:44.509–.569 | 依次进入零扭矩、零速、N、EPB 驻车、人工状态等待 |
| 02:17:44.549 / .569 | EPB 请求 `02 02 02 02`；Shake 中挡位请求为 N (`1`) |
| 02:17:44.589 | 收到新的人工状态、N、零速和四路 EPB=2 反馈，进入 `disarmed` |
| 02:17:44.589 | 最后一批 CAN 把 EPB 改回 `00 00 00 00`、挡位请求改回 `0`；`disarm_complete` 出现在该批发送日志之前 |

源码缺陷是 `Disarmed` 没有延续 `DisarmManual` 的 N/驻车输出，且 close 等待的是
状态机结果，在最后一批 CAN 发送前就可能醒来。修复让 `Disarmed` 保留四路 EPB=2、
N 请求和低握手；close 只有在已退出且对应 CAN 批次全部发送成功后才报告完成。
发送失败或 I/O 异常不能伪装成成功退出。

日志已经报告 EPB 驻车反馈，不能据此断言机械驻车从未执行，也不能断言末帧撤销
就是全部物理原因。修复消除可复现的软件缺陷；实际驻车效果仍须供应方反馈定义
与实车制动状态共同确认。

## 长按前进后叠加左转

`session-000021` 的 `vehicle/runtime.log` 与 CAN 日志：

| 时间 | 证据 |
| --- | --- |
| 02:30:19.360 | seq 1919，D、油门 0.5、转向 -1 被接受 |
| 02:30:19.855 | seq 1927，同样的组合输入被接受 |
| 02:30:20.078 | seq 1931 到达，与上一条有效命令相隔 223 ms；`command_gap_exceeded` |
| 02:30:20.179–.595 | 新命令持续到达，但按住的牵引/转向被 `degraded_neutral_required` 拒绝 |
| 02:30:20.668 | 距最后应用命令 813 ms，独立控制 watchdog 撤销 profile；下一条命令又触发 `session_control_profile_required` |
| 02:30:20.677 | bridge 锁存 `software_fault / session_profile_required` |
| 02:30:21.370 | 浏览器才记录 `driver_input_cleared_on_degraded` |

CAN 中 VMC 故障码为 0，转向已经进入普通控制输出；证据没有显示组合按键导致
ChassisControl 异常。直接触发是控制交付间隔，再叠加等待周期遥测的输入恢复延迟。
已有 trace 显示对应车端控制 mutex 等待为 0 ms；223 ms 间隔中丢失/合并的命令究竟
由哪段网络或调度造成，仍不能仅靠该间隔判定。

修复使用既有 `control_command_rejected` 状态通道，在可恢复的命令间隔或降级拒绝时
立即发送稳定 issue code `control_input_rearm_required`。控制页清除并阻止继续使用
按住的键，保留当前挡位，立即提交零牵引、零转向的新鲜输入。释放后重新按下才
能恢复驾驶，避免在运动中意外请求 N。终端超时不会被提示成可恢复重新准备输入。

硬超时后缺少 profile 的命令仍被拒绝，但不再用 `SessionProfileRequired` 的软件
故障覆盖已有 `Watchdog / OuterControlTimeout` 停车。0.3 → 0.6 → 全量的超时
制动序列继续执行。真实断流或反馈延迟过长仍会安全停车；这次修改没有放宽
200/300/800 ms 门限，也没有让旧按住输入自动恢复牵引。

## 回归与现场复验

软件回归覆盖：完整退出及后续周期的四路 EPB=2/N/低握手；bridge 最后成功发送
批次先于退出完成；60 秒持续前进后增加左转；223 ms 间隔后及时零输入恢复；
持续按住输入不能越过降级门禁；硬超时撤销 profile、保留 watchdog 来源及最终
全量安全制动。首次握手、反馈新鲜度、物理急停、硬超速和重新握手门禁保持原样。

实际验证结果：

- 在 Docker 中将新增回归与未修复 main 的 core、VCU、bridge 源码组合运行，
  三个相关测试目标均按预期失败，分别覆盖末批驻车请求撤销、输入恢复缺失、
  watchdog 来源被覆盖及退出完成早于发送。
- 修复后的完整 CTest 在 Ubuntu 22.04/arm64、Ubuntu 24.04/arm64 中均为
  23/23 通过；`node cpp/tests/control_logic.test.cjs` 为 28/28 通过。
- 实际控制页的 Chromium/Playwright 桌面 fixture（1440×1000）验证前进叠加
  左转、收到重准备提示后立即提交 D 挡零输入、按住键被阻止、释放重按恢复。
  页面身份、非空内容、无错误覆盖层、控制台零错误、截图和交互检查均通过。
  API/WebRTC 使用 fixture，不代表真实运输链路或车端端到端通过。
- `scripts/test/check.sh` 的 Ubuntu 22.04 构建、23 项 CTest、版本与配置检查
  通过，但脚本整体在末尾已有的 Python 引用扫描处退出 2，命中 main 中原有
  admin/build/CI 脚本。本修复不修改该无关门禁；arm64 上跳过的 amd64 runtime
  bundle 检查也不能视为已通过。

现场复验应保留以下独立证据：

1. 在车辆现场安全条件下完成退出，确认零扭矩、零速、实际 N、四路 EPB=2、人工
   状态 3；末批 `0x18FBD0F5` 应为 `02 02 02 02`，`0x18FCD0F5` 应为低握手/N。
   同时确认实际驻车机构与车辆保持状态。
2. 健康链路下持续前进超过 60 秒，保持前进并叠加左转，确认组合输入连续。
3. 受控台架制造短暂控制间隔，确认提示重新准备输入、牵引归零、当前挡位保留；
   释放并重新按下后恢复。另测真正超过 800 ms 的断流，确认 watchdog 停车和
   profile 撤销依然生效。

Docker、fixture 和浏览器测试只证明软件行为，不代表 CAN 实总线发送、执行器、
制动力、真实网络时延或实车接受度通过。修复后需要更新车端及控制端软件再复验。

## session-000031：驾驶端 ACK 误判触发主动重连

本次日志中的控制指令在第一次故障前仍连续送达车端：seq 1815 于
06:41:21.459 被接受，seq 1816 于 .538 被接受。驾驶端在 .463 刚收到推进到
seq 1808 的云端 ACK，却在 .529 报告
`native control signaling acknowledgements stalled for 500ms`。此时最老未确认
指令为 seq 1809，年龄 504 ms，待确认数为 8；距刚收到推进 ACK 仅约 63 ms。

故障判断错误地把单条 ACK 的累计延迟当成整个连接停止推进。主动重连之后，
下一条指令到车端的间隔达到 1042 ms，超过既有 800 ms 安全停车门限。
车端停车来源为 `Watchdog / OuterControlTimeout`；现有日志没有证明是车辆
软件崩溃。云端入队处理与车端控制 mutex 等待均为 0 ms，但原日志缺少云端
实际发送 ACK 的时刻，仍不能定位 ACK 延迟发生在云端发送、代理还是返回链路。

本次修复只改变驾驶端 ACK 活性判断：

- 有待确认指令时，500 ms 内没有有效的递增 ACK 才触发超时。首次等待从实际
  发送完成开始计时；队列清空后的新发送重新起算；断开/重连清除旧窗口。
- 重复、倒序 ACK 不刷新计时；0 或超过实际已发送序列的 ACK 被拒绝。
- 最老指令年龄仅用于诊断。待确认队列最多 128 条；持续落后的 ACK 流达到
  上限时，在发送下一条之前按独立 backlog 故障处理，避免无限增长。
- 真正的 ACK 停滞继续使输入失效并按现有退避策略重连。车端控制交付、
  200/300/800 ms 门限、profile 撤销和渐进安全制动没有改动。

驾驶端 trace/status 新增 `ack_stall_age_ms`、待确认数及上限，ACK 明细记录
`ack_advanced`。这些是 ACK 排空之后、下一次发送之前的快照。云端复用已有
有界异步 trace，新增 `ingress_ack_send_completed / ingress_ack_send_failed`，
记录 ACK 发送开始/结束的 UTC、单调时间、调用耗时和 seq/intent_seq/delivery_cursor。
发送完成表示写入本机传输栈，不能代替驾驶端收到 ACK 的证据。

回归包含 session-000031 的确定性时序重放、499/500 ms 边界、重复/倒序/未来
ACK、空窗后新流量、重连重置及队列上限。真实 loopback WebSocket fixture
通过现有云端服务入队，只延迟返回 ACK：持续推进且最老包超过 500 ms 时保持
连接，停止 ACK 后仍在 500 ms 失效、要求新鲜输入并启用退避。三跳 trace
回归同时检查云端 ACK 发送关联字段与凭证不泄漏。

本次验证：Docker Ubuntu 22.04/arm64 启用
`MINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS=ON` 后，最终源码完整 CTest 为
24/24 通过（便携安全测试包含新增 ACK 回归）；控制逻辑 JS 为 28/28 通过。
仅在 Docker 内恢复旧的最老包年龄超时判断时，新增 WebSocket 延迟 ACK 回归
按预期失败，报告持续递增 ACK 导致主动重连。`git diff --check` 通过；当前
追加修改没有改动车端控制、安全停车代码或配置。未重跑完整 `check.sh`，其
前次已知的 Python 引用扫描失败不在本次修改范围。

此次 ACK 修复必须更新驾驶端；云端更新用于取得新增 ACK 发送日志，不是驾驶端
修复生效的前提。PR 中之前的驻车/输入恢复修复仍需要对应车端及驾驶端更新。
session-000031 的 CAN 末批已有四路 EPB=2，转发驻车 Mode=2；这不是原始
EPB 执行器实际反馈，不能据此宣布机械驻车问题已解决。

## macOS CI：延迟 ACK fixture 的覆盖条件

`aad4067` 的 macOS CI 编译通过，但
`native_control_delayed_ack_progress_and_real_stall` 未满足最老待确认包超过
500 ms 的前置条件。原 fixture 按 80 ms 间隔返回 ACK，依赖发送频率更高来
积压队列；实际发包节奏变慢时，ACK 能追平队列，测试没有制造出目标场景。
在本机 macOS 中把测试发包频率设为 10 Hz 可稳定复现同一断言：19 条指令
已发送、零发送失败、只有一个连接，但最老包最大年龄仅为 309 ms。

修复只调整 fixture：首包 ACK 在收到后等待 350 ms，后续每包等待 600 ms，
以逐包单调时间截止点控制返回。测试使用 10 Hz 验证无需依赖队列增长，仍必须
同时证明 ACK 持续推进、包龄超过 500 ms、未重连，以及停止 ACK 后触发原有
500 ms 失效与退避。失败输出补充包龄峰值、连接数和 native_control 状态。
生产发包频率、ACK 超时、车端 800 ms 门限与 CI 门禁均没有改动。

本机 macOS/arm64 使用 `scripts/build/build_macos_control_bundle.sh test`
通过完整 12/12 CTest，并通过包哈希、静态依赖、JavaScript 语法和运行时 health
检查。本机 SDK 为 macOS 26.5；GitHub runner 的独立验证以当前提交 CI 为准。
