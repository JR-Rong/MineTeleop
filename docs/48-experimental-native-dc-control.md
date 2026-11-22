# PR24 原生 DC 控制实验：默认关闭的第二阶段

第一阶段证据和调用图见 [47-lowest-latency-path-design.md](47-lowest-latency-path-design.md)。本阶段实现可离线审查的原生 DC 普通控制、同语义车辆结果、受 epoch 栅栏的 WSS 回退和管理/状态保底。**这不是所有链路最低延迟目标的验收完成，也不是可直接启用的部署版本。**

`DriverConfig::enable_experimental_control_transport` 默认 `false`，没有 YAML、环境变量、CLI 或网页开启入口；仅隔离测试显式设置为 `true`。旧 Mac、旧车端、未协商版本或没有会话临时 MAC key 均沿用普通控制 WSS。默认网页不轮询新桥。本阶段没有部署、推送、现场驾驶测试或网络配置修改。

## 数据流和权威边界

```text
登录、排他会话、审批、租约续期/撤销、信令、临时密钥：现有云控制面

普通控制：页面 scoped intent → native 唯一全局 seq/时间戳/命令生产者
         ├─ 当前为 WSS：driver WSS → 云 latest mailbox → vehicle WSS
         └─ 当前为 DC：native 最新单槽 → scoped 本机 HTTP courier
                      → 原始 JSON 字节 → 既有 WebRTC DataChannel
         两者 → 同一车端 mutex + epoch fence → 既有 VehicleControlService

车辆结果：同一个软件接受/拒绝结果
         ├─ vehicle WSS → 云独立推送 → native
         └─ DC 命令另回同一 DC → courier → native 验证/精确匹配

状态/管理：沿当前 ordinary path；DC 断开后协商 WSS 保底
         4 类状态最新缓存 → 云 → native → 原页面 reducer
         profile/VCU 管理 → 原认证信令 → vehicle（独立于 ordinary seq）

急停：native → 现有 WSS；不参与 ordinary DC 优选
视频：既有同一 WebRTC PC，ICE all；本阶段未改变 ICE 提名或视频选路
```

DC 可能是 P2P，也可能是 TURN/UDP、TURN/TCP/TLS；`dc` 不是 `p2p` 同义词。直连成功需要 selected pair 和真实帧/结果推进证据。车端结果是共同软件服务结果，不是执行机构/CAN 最终执行确认。

会话创建仅在显式请求新协议时签发会话级临时 HMAC-SHA256 key。native 私有保存，车端从现有认证 discovery 获取；普通会话序列化、本机 connect/status、日志不输出 key。关闭会话清除 key。没有新增持久凭据。command/result 使用不同 MAC domain，覆盖原始命令语义/epoch；浏览器只转送 native 序列化的 `frame_json`，不拥有 key，不能自行生成控制包或伪造车辆结果。原始字节转发也避免 JS 将 `0.0` 转成 `0` 后破坏签名。状态仍信任现有认证车辆信道，不作为 native 控制结果计分。

## 切换、健康样本与有界回退

- native 全会话序号不重置，ordinary 只发一路，桥只保留最新一包。页面、session generation、UI owner/epoch、media attempt 不匹配的请求被拒绝。发送过期、页面冻结、bufferedAmount 堵塞时不追发积压包、不重写源时间。
- `select` 绑定 `from_epoch → epoch=from+1`、path、media attempt 和最小 command seq，必须 neutral。车端确认后才清除 selecting；重复选择幂等，旧 epoch 即使 seq 更高也不应用。selection ACK 丢失后只从精确匹配且认证的 conflict result 对齐实际车端 epoch。
- 切换、资格验证、反馈恢复均撤回运动、失效旧输入；车端调用既有 braking wait。selection 同时清除 control-link-ready 和 operator-input-ready；恢复仍经过既有健康样本、profile、VCU、视频、时钟、输入租约和松开重按门禁。
- 用真实 WSS 车辆结果均值建立基线，DC neutral 取得 3 个新鲜 accepted 结果后才比较。DC 均值不优于 WSS 则回退；WSS 已在原 500ms stall 边界内无真实车辆进展时可只开始 neutral DC 试验。ICE RTT、云入口 ACK、被拒绝但返回很快的结果不算驾驶资格。
- 每 media attempt 默认试验一次；WSS 确实停滞后才允许按恢复等待周期重试。DC 断开、attempt/owner 失效、桥失新鲜、实际结果停滞触发新的 WSS epoch，不将旧 DC 包改标签后补发 WSS。
- 结果精确关联 command seq/intent/stamp，第一次匹配计时并消费；DC/WSS 重复回报不再次计分或刷新 watchdog。记录 `(command_path, feedback_path)`。测量 native publish/send start → native 收到车端软件结果，包含本机 courier、去程、处理和回程。

这是一轮 neutral 验证加故障回退，不是持续全路径最优算法：活动路径尚健康但逐步变慢时不会持续探测、重排所有替代路径。第一阶段 advisory policy 的改善幅度/驻留/稳定窗口尚未接入这里；不能声称已完成实际切换滞回的全场景验收。

## 所有时限取舍

| 数值/来源 | 本阶段用途和限制 |
| --- | --- |
| 新增 25ms 本机 courier tick | 实验开启时最多一个 HTTP in-flight；页面调度抖动计入实际结果。不是安全 watchdog。默认关闭不发送该请求。 |
| 新增 100ms native 单槽 publication allowance | 保守丢弃过旧的本机待转发包，剩余有效期扣除本机请求耗时；不是云失联阈值，也不是由 command max-age 推导的 cloud timeout。车端仍独立按源时间做原 max-command-gap 检查。 |
| 既有 ACK fresh 200ms、3 个异常、stall 500ms | 实验 DC 结果恢复复用这些边界；度量改为真实车辆结果，不能视为已现场批准的性能预算。没有放宽原安全阈值。 |
| 3 个 accepted neutral 新鲜结果 | DC 资格与均值比较；没有绝对/相对改善阈值，不支持统计上证明微小差异。 |
| 既有 profile control-recovery-timeout，默认 5000ms | selection/资格截止与失败重试防抖；原配置范围不变，不能当云断线允许继续行驶时长。 |
| 128 个发送结果记录；4 类状态各保留最新一条 | 固定内存，淘汰只影响可度量记录；状态按 control_status_seq 排序交既有 reducer。 |
| 既有安全退出 15s 与最终 CAN 条件 | WSS 管理 HTTP 成功只表示请求完成；必须等待本次退出后新鲜、完整、匹配会话的最终 CAN 状态。epoch 或会话变化不能确认成功。 |

## 启用前必须决定：云权威失联语义

现有实验 fence 要求车端 control WSS 仍为 connected、云声明新协议能力、已知控制租约未过期；现有信令/HTTP错误仍 fail closed。租约更新通过现有 WSS 在 expiry 改变时通知，没有新周期 heartbeat。**静默黑洞中 connected 标志可以滞后；现有默认 control-token TTL 是 5 分钟，不能把它当合适的短失联停车保证。** DC 命令仍新鲜且结果仍推进时，单凭 command max-age 不会发现云撤权消息无法到达。

| 待比较方案 | 代价与未决点 |
| --- | --- |
| 最小 lease/revocation/session epoch 扩展 | 云签发有界 authority lease，绑定 session/driver/vehicle/authority epoch，车端仅接受该 epoch 且未过期；撤销推进 epoch/关闭授权，续期通过既有控制面。可减少新流量，短云抖动可在明确许可的 lease 内继续；失联最坏界是 lease 剩余时长加可信时钟误差。必须由主线程确定 lease 长度、续期间隔、撤权时限、时钟不可信处理与断网是否允许继续。不能沿用 5 分钟 TTL 充当短安全界。当前 MAC key 不是 cloud-signed lease，也未实现这些新语义。 |
| 更强的周期 authority heartbeat | 云/车/native 认证的单调进展，过期撤回输入并停车；更快发现静默黑洞，但云 RTT/调度/短丢包会让健康 P2P 也停车，增加流量和共享云依赖。先评估各地 RTT 分位数、误停率与故障检测需求，再决定周期/截止/连续丢失策略。 |

**未实现、未批准 50ms heartbeat 或 200ms cloud-loss gate。** 200ms 命令时效和 200ms ACK freshness 均不是云失联检测要求。对云短暂不可用而 P2P 健康的取舍需要主线程/用户决定；此前保持内部开关关闭。

## 离线验收和剩余缺口

| 验收 | 已做 / 仍需做 |
| --- | --- |
| 协议版本、身份/token、过期/未来包、租约过期、旧 attempt、MAC篡改/反射、密钥不公开 | 共享 C++ 协议测试；测试向量为公开 fixture，不使用现场凭据 |
| neutral selection、幂等重复、旧 epoch 高 seq、丢 ACK reconciliation、WSS ESTOP、去重 | 共享 fence/sender 测试；含真实 LogicalTimeControlService watchdog/恢复语义 |
| 单槽原字节、旧页面/attempt、过期不追发、单 HTTP in-flight、重复结果 | JS courier 测试 |
| 真 native/cloud HTTP+WSS、真实 vehicle-result 延迟、fake DC、epoch WSS 回退 | localhost integration；DC 是字节 courier fixture，未运行真实 WebRTC/GStreamer/硬件 |
| WSS 安全退出仍等本次最终 CAN、epoch 变化不能确认 | 实际页面函数 mock；不以 HTTP 请求成功替代退出证据 |
| 默认关闭、旧 WSS ACK、ICE signaling/页面、既有控制逻辑与固定离线回归 | 本机 native 构建与指定测试，完整回执附交接 |
| Linux 完整车端编译、真实 DC/P2P/TURN 和关闭/重连并发 | **未完成。** Mac 无 gstreamer-webrtc pkg-config；既有 Docker image 只读/无网启动失败于 daemon containerd 临时挂载 I/O，未 reset/delete。交由 PR24 Linux CI 及协调台架窗口，不在台架编译。ICE 方法提取 mock 的通过不等于完整车端编译。 |
| 反馈独立最低健康延迟选择、急停最低路径/同 seq 冗余 | **未实现。** 状态跟随 ordinary path；DC 命令结果可同时经 DC/WSS 回报并首次精确匹配消费，但没有独立反馈择优器。急停仍固定 WSS。 |
| 视频最低端到端延迟提名与真实直连失败根因 | **未解决。** 本阶段只保留 ICE all 和诊断，不能以媒体 RTT 替代控制结果/视频 capture→presentation。 |

复现：构建 `mine-teleop-control-transport-tests`、`mine-teleop-control-transport-integration-tests`、`mine-teleop-native-control-ack-tests`、`mine-teleop-ice-signaling-tests`、`mine-teleop-latency-path-tests`；`ctest` 用明确 suite 名，仅 localhost socket fixture。`python3 scripts/test/run_offline_regressions.py` 是不占台架的固定脚本。所有本地 socket 测试清除 `MINE_TELEOP_*` 和 loader/test overrides。

有界探测补充（经主线程单独批准，窗口已释放）：2026-10-15 06:07:41 UTC Mac、06:08:14 UTC 台架，各仅对既有云 STUN UDP3478 同 socket 3 次事务、每次 1s；两端均超时。既有 TCP443 各一次合法 Binding 事务成功，Mac 22.737ms、台架 63.464ms。不是 TURN allocation、不是全 UDP 阻断证明、未改网络/服务。白名单结果和退出码随交接，未保存映射地址、事务ID或原始 SDP。
