# 原生 ACK 制动等待与 session-000076

## 运行规则

原生驾驶端 ACK 连续 500 ms 没有合法递增进展时，先清除旧运动、冻结当前挡位，
持续发送零牵引、零转向及 `control_link_ready=false`，保留当前 WSS、会话和已确认
profile。车端将该标志作为独立安全制动请求；黄色条显示“控制链路 ACK 确认暂时
中断”。本地标志只能证明已请求制动，车端反馈才证明软件制动状态。

恢复需要真实已发送命令的 ACK：确认对应包往返年龄不超过 200 ms，继续有新的
递增确认，健康观察至少 300 ms。重复 ACK、旧积压 ACK 或半帧不能确认恢复。
进入和退出等待都使旧运动输入失效；必须提供新的零运动输入、释放重按，才能在
原会话继续控车。等待中的中立刷新不能提前授权运动，正常持续推进的 600 ms
返回 ACK 也不会仅因历史包年龄超过 500 ms 而主动重连。

等待上限使用已经完整协商的 V7 `control_recovery_timeout_ms`（默认 5 s、控制端
可配置 1–30 s），无需新增参数或改变 17 字段 profile。超过期限才重新建立控制
传输；真实 socket 读写/协议错误及 128 未确认包上限仍立即按既有退避重连。
驾驶端每次新传输的等待窗口不延长车端首次会话故障截止点，不能解除硬停车锁存。

持续没有合法且成功应用的控制仍按车端默认 800 ms、现场配置 2000 ms 停车。
这不是“不按键”的门限；正常有效零心跳可以续期。断网时发不出去的零命令、
拒绝的运动/过期帧和本地制动均不能续期。若 ACK 返回路径单独受阻而零命令仍能
到达车端，可制动等待；若整个链路中断，车端独立 watchdog 优先硬停。

## 三端兼容

云端 `/health`、已授权车辆目录和原生 WSS 的 101 响应公开能力标志
`v7-feedback-ack-wait-v1`。驾驶端在创建/切换控制权前验证 HTTP 目录，并在发送
普通原生命令前检查实际控制 WSS 后端，覆盖 HTTP/WSS 路由不同版本的情况。
缺少能力时明确提示更新云端，不静默进入缺少恢复反馈的控车链路。

普通 `control_link_ready=false` 同时镜像为既有必需的 `operator_input_ready=false`。
旧 V7 转发器即使丢弃新的原因字段，车端仍会因输入未就绪制动，避免执行运动。
`false` 链路标志搭配 `true` 输入标志的普通包直接拒绝。新原因字段缺省为 true，
完整 profile 协商仍保持原 V7/17 字段、逐项精确 ACK；V6 普通控制依旧拒绝。
已鉴权急停不依赖新增能力标志，安全退出保留 HTTP/普通信令路径。

现场应更新同一提交的驾驶端、云端和车端，重新打开页面并保存各包 manifest 的
`source_git_sha`。页面 hash 或 V7 参数确认不能证明云端原生反馈实现已更新。

## session-000076 的停车结论

以下时间均为 UTC 2026-10-13。附件 CRC/JSONL 均正常，但 manifest.complete=false；
CAN 达 64 MiB 导出上限，只覆盖 08:19:53–08:24:13，不含最后停车，无法验收最终
物理制动/驻车。页面 hash 对应 55a4ee9/956337f 的相同页面代码，不是 8d13c23；
不能由页面推断整包 SHA。现场参数为视频 500 ms、恢复 5000 ms、控制 watchdog
2000 ms、命令年龄 200 ms、连续迟到 3 帧。

| 时间 | 原日志位置 | 事件 |
|---|---|---|
| 08:32:52.452 | vehicle/runtime.log:2822 | seq12556、intent253，最后合法运动成功应用 |
| 52.586 | runtime:2820 | critical channel0 编码帧年龄 522 ms，采集年龄 10 ms，进入制动等待 |
| 52.667 | runtime:2822 | seq12557 迟到 230 ms；后续旧非零运动被 `degraded_neutral_required` 拒绝；车端发送 rearm=true、status_seq9500 |
| 52.809 | cloud/signaling-audit.jsonl:1948 | 云端收到 seq12558 delivery ACK，但记录缺少反馈字段 |
| 53.336 | runtime:2828 | 视频在 750 ms 后恢复，仍保持制动并等待新零输入 |
| 54.452–54.544 | runtime:2832 | 无合法应用命令达到 2 s，profile 撤销；首次硬停车 tick 在旧日志中缺失 |
| 55.251 | controller/control-browser-events.jsonl:6617/6618 | 浏览器才收到 DEGRADED 并清除仍按住的方向键，晚于硬门限 |
| 58.544 | controller:6647 | 明确显示 watchdog/outer_control_timeout |

最终窗口内有 29 次车端命令回调，最大间隔 125 ms，除一条 230 ms 迟到外其他
年龄 40–168 ms，control mutex 等待均为 0。全会话 ACK stall 最大 315 ms，没有
send_failed/重连。因此最终停车不是 ACK 500 ms 重连，也不是连续 2 s 没收到报文。
车端拒绝旧运动且不为它续期符合预期；恢复反馈传递失败使本可恢复的等待变成硬停。

全会话驾驶端 14221 个 ingress ACK 都没有车端原生反馈；云端 13927 条 delivery
ACK trace 都缺少 `vehicle_feedback_present`，没有已有源码的 ingress ACK 写阶段。
与旧云端实现或其他转发路径丢弃反馈一致。当前源码已有反馈验证、存储、返回和
驾驶端重新输入处理，但附件没有云端二进制 SHA，不能确认具体现场版本，也不能
宣称发现当前云端同一路径的新源码 bug。新增能力检查用于阻止该兼容缺口。

80 次带有效 profile 的软件制动等待中，72 次视频、6 次输入租约、2 次校时；
另 5 次是硬停后 readiness 变化，不是新的驾驶停车。不能等同 80 次物理停稳。
五路采集没有 failure/reopen/sequence_gap，采集间隔最大 43–45 ms；channel0
编码背压计数 7864，采集到编码历史最大 2883 ms。驾驶端选中 TURN/TCP，采样
RTT 412 ms、jitter buffer 220 ms，解码约 3 ms。更支持编码/媒体交付积压，未见
硬件采集断开的证据；现有日志仍不足以区分网络重传和软件调度的具体贡献。

## 新增诊断及判定边界

- 复用有界异步 trace，每秒 `details.observations` 独立观察当前阶段和停留时间：
  driver sample/connect/ACK drain/send；cloud receive_frame、loop/ingress mutex、
  权限检查、vehicle delivery send、ingress/vehicle ACK 写。即使 handler 未返回，
  仍可采样；`phase_snapshot_stable=false` 不报告伪精确耗时。
- 观察真实连接的 TCP RTT/重传、待发/未读字节、peer 和连接开始时刻；驾驶端锁
  被占用时报告 `control_socket_lock_busy`，不会等控制锁。弱引用登记有界，连接
  关闭前 detach 防止 FD 重用混入新连接。支持边界见 [诊断字段](24-vehicle-runtime-diagnostics.md)。
- `observer_heartbeat_gap_ms` 检查观察线程本身的停顿。它与既有异步日志输出共用
  worker；磁盘/日志输出阻塞也可能推迟采样，不能把日志缺行单独等同断网。
- `ack_braking_wait_entered/recovered`、原生 status 的 deadline/packet age，以及
  `cloud_capabilities_verified` 和云端启动能力标志说明恢复和实际后端校验状态。
- 车端 `control_hard_stop_transition` 在 tick 或 receive 导致的首次硬状态观察时
  保存最后解析到的命令及年龄、最后进入核心校验的 seq/结果、最后接受的 seq/年龄、
  状态反馈 seq、累计接受/拒绝、watchdog tick/跳过周期和 WSS 最后报文时间。
  “已解析”不是鉴权或接受证明；只有 `last_accepted_*` 能证明合法应用。日志动作
  不修改控制资格或门限。

## 验证与未验收项

本轮 Docker Ubuntu 24.04/arm64、GStreamer 1.28.5 完整构建与 CTest 24/24 通过
（80.30 s）；控制集成 47 项、核心 78 项、portable 输入/ACK 9 项、控制 JS 48 项。
最后采样限频调整后再次构建，针对控制集成和控制 JS 两项 CTest 通过。

Docker 真实云端 WSS、驾驶端和车端控制服务/mock 夹具覆盖：返回 ACK 600 ms
持续推进、ACK 真停滞先制动且连接数不增加、短波动恢复后原 profile 继续控车、
旧长按不得自动复活、新中立/新运动、持续不恢复才有界重连。核心输出历史覆盖
控制链路与时间/输入/视频原因，独立安全制动及 799/800 ms 硬边界。

兼容夹具覆盖旧 HTTP 后端不建立权限、HTTP/WSS 不同后端不发普通控制、能力
检查失败后新后端可连接、已有会话的旧后端仍接收鉴权急停及安全退出。观察夹具
主动保持半帧和互斥锁等待，验证操作返回前已有独立采样。首次硬停车字段由生产
源码诊断合同检查，不宣称已经通过实车运行验证。

未部署或操作现场车辆。软件夹具与跨平台 CI 不能替代网络、媒体容量、VCU/EHB/
EPB 执行及实车验收；下一次需同提交三端完整日志和最终停车时段 CAN。
