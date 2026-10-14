# 输入刷新、视频时延与 session-000008 排查

## 日志结论

附件 `service-badd67962af55b9339177b0d_session-000008.zip` 的 JSONL 均可解析。
会话约 194 s。manifest.complete=false，驾驶端上报有日志丢弃，CAN 导出存在扫描/大小上限，
不能把缺少记录当作“没有事件”。没有原始视频、双端抓包或现场驱动/路由器数据。

在控制 profile 仍有效时，记录了 41 次进入可恢复制动的状态转换：
33 次 critical_video，其中 23 次驾驶端关键画面健康为 false、10 次车端输出端不新鲜；
8 次 input_lease。另有 12 次视频 readiness 变化发生在 profile 已失效后。
这些是保护状态转换次数，不能据此断言车辆完成了 41 次物理停车。
驾驶端有 24 次本地输入 HTTP 超时，最长约 434.5 ms。

首次硬停车的控制决策来自有效控制超时：

| 控制序号 | 车端结果 | 驾驶端到车端回调 UTC 差值 | 驾驶端发出到云端接收 UTC 差值 |
| --- | --- | ---: | ---: |
| 2607 | accepted | 192 ms | 146 ms |
| 2608 | command_age_exceeded | 215 ms | 163 ms |
| 2614 | command_age_exceeded | 243 ms | 230 ms |
| 2623 | command_age_exceeded | 436 ms | 379 ms |
| 2634 | command_age_exceeded | 508 ms | 455 ms |

runtime.log 第 756 行的 control_hard_stop_transition：最后有效命令 2607，
最后判定 2634，连续迟到计数 5；最后有效控制年龄达到本次现场 2000 ms，进入 TIMEOUT_BRAKE。
命令一直到达，但不满足当前 200 ms 包年龄要求，不得执行，也不得续期有效控制看门狗。
这是当前保护规则的预期结果，不是“不按键两秒”或“单帧迟到立刻硬停车”。

该窗口驾驶端 sender 唤醒延迟最大 16 ms、send 调用最大 3 ms、采样处理最大 4 ms；
车端控制锁等待 0 ms、接收路径处理最大 1 ms；云端 mailbox 等待最大 49 ms。
增加的延迟主要位于驾驶端实际发送到云端接收之间。跨主机 UTC 差值含校时误差，
这里的持续增长远大于当时几十毫秒的不确定度，但仍不能排除接收前的主机/代理调度。
没有证据证明“整个网络断开”；排队、传输退化和主机 recv 前等待均可能贡献。

CAN 第 205234 行锁存 watchdog / outer_control_timeout，随后 control_apply_timeout
报告 last_apply_age_ms=2016、timeout_ms=2000。不要把后一个桥接 watchdog 当作首个原因；
也不要把 CAN ts 与 runtime 同步事件时间不加核对地拼成精确执行耗时。
驾驶端 ACK wait 在更晚的约 +184 s 出现，约 +189 s 达到恢复截止点后重连；
当时 TCP RTT 约 1.298 s。+192 s 媒体连接 failed，+194 s 为 driver_logout。
这些后续事件不能倒置为 +165 s 首次硬停车的根因。

## 软件修复及边界

- 浏览器原先单个 async drain 等待 HTTP 响应才发送下一次心跳。
  现在普通请求最多并行两个，一个 latest-only 待发状态，急停保留第三个位置。
  新输入边沿在槽位满时中止最旧普通响应；心跳只保留一个待发状态、等原响应期限，
  避免抢占仍有效的 132–150 ms 回复/挡位确认。每次派发重新读取当前输入，不积压重放旧长按。
  急停不被普通心跳中止；会话切换会撤销旧槽位并丢弃旧 pending 急停/输入。
- 乱序响应按请求会话、generation、intent_seq 和取消状态验证。
  已被替代的重激活拒绝、鉴权错误或挡位结果不能覆盖新的输入/会话状态。
- 本地 HTTP 完整请求的实际接收时刻作为原生输入租约起点；应用前已超过租约的普通
  请求被 local_input_request_expired 拒绝。较旧的同 UI 到达时刻不回退/重续租约。
  合法急停仍可抢占，不能因普通输入年龄检查丢弃迟到的急停。
- 视频 totalProcessingDelay 覆盖首次 RTP 到解码完成，已经包含接收端缓冲等待。
  新计算不再重复加 jitterBufferDelay；无此统计时使用 buffer + decode 的较窄近似，
  缺失/非法计数或时延保持 unknown。该估算不是实测端到端时延，也不参与停车门禁。
  参考 [W3C WebRTC Stats](https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-totalprocessingdelay)。
- 轨道重新 attach 清空旧恢复计数，仍需三个新呈现帧，反复采样同一帧不会增加计数。

不改变 V7/17 必需控制字段、profile 协商、25 ms 校时门限、默认 5 s 恢复截止点、
200 ms 浏览器输入租约、200 ms 包年龄及车端默认 800 ms/现场 2000 ms 有效控制看门狗。
本修复不添加“禁用安全停车”开关。主要运行修复在驾驶端，新日志也在驾驶端；
无需为新增必需协议字段同时更换云端和车端。现场应验证驾驶端新包/页面源指纹，
保留已有协商匹配与真正断流保护。

## 为什么使用中转

现场说明：驾驶端与车端当时均为电信 5G，未开启已知 VPN 或系统代理。
双移动网的 NAT 行为可能阻止候选互通，但现有记录不能确定 NAT 类型。

当前仓库的驾驶端、车端默认 ICE policy 为 all，现场 YAML 同样为 all；all 允许直接
候选和 relay 候选参与连通性检查，不能保证直连一定成功，也不强制 relay。
session-000008 的选中链路 local relay / remote relay，驾驶端 relay_protocol=tcp。
协议字段 udp 指候选上的传输，不能据此说客户端到 TURN 使用了 UDP。
驾驶端事件 9/10/16/17/29/30/398/399 对 STUN/TURN 记录了 701。
[W3C 定义](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnectioniceerrorevent-errorcode)
说明这是无法通过 host 候选到达对应 STUN/TURN URL 的错误。STUN 的默认 UDP URL
失败支持 UDP 服务不可达的线索；旧 endpoint 日志没有保留 TURN transport 查询参数，不足以唯一证明 NAT 类型、全部 UDP 都被阻断或车端具体防火墙规则。

旧日志未记录所有直接候选对的请求/响应结果，也没有证明现场驾驶端启动参数未覆写
ice_transport_policy。新增日志记录实际页面配置和候选对状态，后续区分“没有 srflx”、
“有候选但检查无应答”和“直连成功而未选中”。不通过行驶中重新协商改变握手连接。

当前普通控制命令仍固定经 driver -> cloud -> vehicle 的原生 WSS 转发；选中 ICE
链路描述媒体/媒体 DataChannel，不是普通控制 WSS。视频直连不会自动改变控制路径。

另只读核对了当前云端：TURN service active、UDP 3478 在本机接口监听；本次 Mac
发送 STUN binding 到公网 UDP 3478 两次均在 3 s 内没有响应，INPUT policy 为 ACCEPT。
这不等于云安全组/其他防火墙均已放行，也不代表现场 Windows/车端网络。需要结合
新候选检查日志、现场 DNS/UDP 可达性与云安全组才能定位；未修改生产防火墙。

## 回归验证范围

原生测试只在 Docker 内构建和执行。新增测试覆盖输入本地过期、到达乱序、松键后旧运动
不能恢复、迟到急停不被普通年龄规则丢弃；既有一分钟长按及真实失租重激活测试继续保留。
JS 回归覆盖慢响应与心跳并行、单个 pending、急停优先、旧会话 pending 丢弃、乱序响应、
视频统计缺失与重复计时、关键画面各类原因、重复采样不增加恢复帧和日志频率上限。
ICE 诊断输出最多 16 对，并保证选中对/直接检查优先、未包含地址/凭证。

浏览器使用 Docker 构建的原生页面、隔离 API/peer fixture 和真实 canvas MediaStream。
Browser plugin not available，因此使用现有 Playwright/Chrome；不连接实车或生产云端。
120/145/220 ms 延迟响应下验证长按、释放后继续转向、关键画面冻结、恢复不复活旧输入及急停。
桌面与移动视口检查、截图和无应用 console error 仅证明 fixture UI，不能代替实车、
真实链路及 CCG2 画面质量验收。本次改进可消除软件排队/重复计时并提高归因能力，
不能承诺覆盖 session-000008 中持续过期命令的真实传输退化。

复核还修复了 native_driver_to_vehicle_signaling_control_payload 的测试域混用：
驾驶端发送时戳来自已接受的云端锚点，而夹具用原始宿主 UTC 接收；Docker UTC 回调
可能造成偶发未来年龄。该 payload 夹具使用明确的逻辑接收点，并增加未来/过期拒绝
断言，生产规则不变；真实三端 steady/校时测试仍保留。一次四时戳一致性 fixture
失败在单项复测通过，未因此放宽生产校时验证。

macOS CI 的 ACK 延迟和原生恢复 fixture 原先把模拟车端接收与驾驶端状态查询
放在同一线程，且使用原始宿主 UTC。补车端校时后，复测仍出现年龄 533 ms、
有效控制 2562 ms 的真实拒绝/超时，说明单改 UTC 域不足以隔离测试自身的排队。
两项 fixture 改为独立车端接收线程，状态断言通过同一互斥锁观察；接收连接建立后
才启动 service/profile 的时间边界，车端也同步同一云端。保持真实三端 WSS、默认
steady_clock 看门狗与 200/800 ms 原门限，线程异常回传测试，不忽略过期包。
350 ms 校时处理延迟测试改为要求完成的慢采样组仍满足 25 ms，而非要求每个中间
快照持续可信；真实 UTC 跳变仍可正确使单组采样失信，必须在期限内重新通过。
失败信息包含 driver/vehicle 校时及 native sender 状态，不放宽生产安全断言。

原生恢复 fixture 分别观察自动保留制动与新 UI 输入恢复，不把两个阶段混为一次
观察。失败时携带原生输入状态、最近 8 个车端命令，以及首次 TIMEOUT_BRAKE
之前的拒绝原因/有效控制年龄，避免只记录硬停后的 profile 已清除。保持实际
制动、旧运动拒绝和新输入恢复断言，Docker 控制测试复测通过（39.94 s）。

校时中位数误差回归用实际回复边界计算合成中点，并采用明确的 500 ms 合成
偏移差，仍要求 25 ms 信任门限拒绝且 uncertainty 覆盖 best RTT/2 + median shift；
不把名义 sleep 时长视作实际网络 RTT。登录限流 fixture 的锁定窗口改为 1000 ms，
再验证窗口内拒绝及窗口后恢复，避免 25 ms 窗口在审计 I/O/断言前已正常到期。
这两项只调整测试数据/计时，生产校时与登录配置均不改变。
