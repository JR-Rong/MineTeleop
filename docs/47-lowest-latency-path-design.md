# PR24 最低健康延迟路径：证据、实施设计与验收

基线 `2cae265cd350776bdbbe15bc495a281a9b805e6d`。本改动只在本机隔离检出开发；没有部署、操作 Mac UI、连接台架、改变网络或修改既有工作树。所有后续集成仍交主线程进入 PR24，不另建 PR、不合并、不 force push。

## 已确认的调用图

```text
云控制面：登录/车辆授权/排他会话/手机审批/控制租约/撤销
        └─ 认证 HTTP/WSS 信令 → offer/answer/trickle ICE/临时 TURN 配置

视频：vehicle capture → encode → RTP → 同一个 WebRTC PeerConnection
      → 浏览器接收/缓冲/解码/呈现
      ICE all 允许 host/srflx/prflx 直接路径，也允许 TURN

普通控制、急停：页面 scoped intent → Mac C++ 唯一周期命令生产者
      → driver send-only WSS → 云 latest mailbox
      → vehicle control WSS → 共同 VehicleControlService → safety/adapter

入口确认：云入队 → signaling_ack → Mac；不证明车端收到
投递确认：vehicle → signaling_delivery_ack → 云；不证明接受/执行
车端结果：VehicleControlService → control_feedback → vehicle WSS
      → 云保留最新结果 → 以后 driver signaling_ack 捎带 → Mac

管理与遥测：profile/VCU handshake 浏览器 DC → vehicle
      vehicle telemetry/profile/VCU/rejection → 同一个 DC → 浏览器
```

车端 `webrtc_media.cpp::handle_control_message` 显式拒绝 DC 普通 ControlCommand。DC open 不等于控制直连。DC 与视频共享 PC/ICE，实际同样可能走 TURN。DC close 当前会关闭控制服务并清 profile；不能在这套生命周期下声称 WSS 是独立、完整的驾驶保底。所有输入租约、序号、身份、时效、readiness、恢复 revision、松开重按、独立 watchdog 仍须共用。

## 最具体的直连证据与缺口

证据根目录（只读）：`/Users/rongjianrui/Documents/Codex/2026-10-13/task-2/pr24-review/implementation-evidence/direct-e2e-20261015/`。

| 证据 | 观察 | 能证明什么 / 尚不能证明什么 |
| --- | --- | --- |
| `diagnostic-live-20261015/2cae265/window-004/ice-readonly-current.json`，同一 PC 两次相隔约 1 秒 | transport 明确选中 relay→relay；本地 relayProtocol=tcp；RTT 54/49ms；RTP/解码/DC接收计数推进 | 真实 TURN/TCP 可用。浏览器 getStats 本地仅 host/tcp×2、relay/udp×2；远端仅 relay/udp×1；晚时点统计表只有这一候选对，未保留其他 checks；不能证明历史从未检查。后续同一Peer的SDP摘要已证明host候选进入交换后描述（见下），缺早期逐对检查历史，不能推断丢在哪层或 UDP 全阻断。 |
| `run-003-all-host-only-crlf/execution-manifest.json` + `result.json` | 37.221 秒，ICE all，但 ICE server 数=0；车端产生 host/udp×8、host/tcp×16；浏览器 2 个本地候选已发送，24 个远端候选已应用；部分 host UDP checks 至 41 次请求、0响应；无 DC/帧回调/selected pair | host-only fixture 在此拓扑内未成功。srflx=0 是没有 STUN 配置的预期，不能由此归因产品 STUN 配置。不是含 TURN 的完整产品 all 测试。 |
| `mac-stun-binding-probe.json` + `bench-stun-binding-probe.json` | 既有服务 UDP3478 单次 Binding 各约3秒超时；TCP443/6000有合法 Binding 回复 | 仅该目的服务/端口/时间窗未收到 UDP 响应。未做认证 allocation，不是 TURN/UDP、媒体或总体 UDP 可达性证明。 |
| 既有 seq3564/3565 单调 trace | 云写至车端 ACK 3702/3661ms（交接事实）；入口 ACK 仍快 | 云入口健康不能代表控制端到端健康；TCP_NODELAY 后短窗口未复现不能证明 Nagle 根因。 |

后续交接 `diagnostic-live-20261015/DIRECT-ICE-EVIDENCE-HANDOFF-20261015.md`（05:29 UTC）提供更强的同Peer当前SDP类型摘要：Mac local answer 有 host/udp/mDNS×2、relay/udp/private×2；remote offer 有 host/udp/IPv6×6、host/tcp/IPv6×12、host/udp/privateIPv4×2、host/tcp/privateIPv4×4、srflx/tcp/otherIPv4×4、relay/udp/privateIPv4×4。两端实际配置与PC均为all，gathering complete；旧003车端relay需分开。**host UDP和TCP srflx已经进入交换后的描述，两份描述均未见UDP srflx**；不能由晚期getStats表缺项声称未生成、未交换或被全部过滤。原始SDP未保存；本任务复核的是白名单交接，不声称独立读取了原始SDP。

同一交接的进程白名单未发现Chrome显式禁用WebRTC UDP参数；不排除管理策略。`diagnostic-live-20261015/direct-listener-preflight.json`（05:27:47 UTC，本任务已读取）确认云UDP3478在private/loopback监听，UDP16000和TCP6000/16000在wildcard监听。监听存在不等于公网UDP路径可达，VM私网绑定也不单独证明配置错误；relay地址类型private同样不能单独认定external-ip配置错误。

当前最具体缺口是**UDP STUN事务没有可确认回复、没有UDP srflx，以及真实产品早期直连pair检查历史未闭环**。两端访问共同云端UDP3478均超时与缺UDP srflx相容，但尚不能区分云公网入站/安全组/NAT映射、服务应答路径、终端NAT/网络策略或软件时序。优先提取已有本次attempt的 `driver_ice_connectivity` 和 `webrtc_ice_candidate_error`，再对既有端点做有界事务ID匹配Binding及云服务本地自测；先由主线程安排台架空闲窗口。不要先延长安全超时、强制直连、删除 TURN 或改防火墙/VPN/路由。

## 三类链路的真实能力和测量

| 链路 | 当前可用/已证明路径 | 应用于最低延迟比较的度量 | 回退边界 |
| --- | --- | --- | --- |
| 视频 | WebRTC；真实 TURN/TCP 已证明；P2P、TURN/UDP 尚无成功实证 | 每路 capture→presentation，带源帧身份、可信时钟/不确定度，帧推进/丢包/冻结；ICE RTT只能是网络诊断 | 同一 ICE stack 的 all 候选与 TURN。当前没有 WSS 视频回退。视频不健康仍须安全停车。 |
| 控制 | 当前仅 cloud WSS；DC普通控车未开放 | 唯一 native 命令 seq/intent → 车端软件结果 → native 消费的同机单调 round trip，记录命令/结果各自路径 | 新协议才可在 DC/WSS 间选择单一 ordinary 路径。WSS可达不免除视频、profile、VCU和输入恢复门禁。 |
| 反馈 | DC 遥测/管理状态；WSS车端结果捎带回传 | 同一语义/身份的车端结果回传；遥测需源序号、源时间、接收时间、时钟可信度及更新间隔 | 两条现有消息不是等价ACK，不能直接比较。需要 WSS 管理/遥测独立推送和 DC 相同车端结果事件。 |

`protocol=udp` 的 relay candidate 描述 ICE candidate，不能代替 client→TURN 的 `relayProtocol`。至少一个已知 TCP/TLS relay leg 分别标为 `turn_tcp`/`turn_tls`；所有 relay legs 明确为 UDP 才标 `turn_udp`；任一未知保持 `turn_unknown`。缺候选类型时不能标 P2P。`STUN` 旧显示标签是候选来源，STUN 不转发媒体。

## 本提交实现的有界第一阶段

1. `VehicleResultLatencyWindow` 独立保留128个成功发送的 command_seq、intent_seq、send-start单调时间。云入口 ACK 不退休这些记录。同会话 socket reconnect 保留，session reset 清空。只有匹配的 advancing 车端结果生成样本；重复/倒序、未知/淘汰、intent不符、时钟倒退不造健康样本。固定数组满时只淘汰诊断记录，不妨碍 ESTOP 或发送。
2. native status 新增 `native_control.vehicle_result_latency`；trace 新增 `vehicle_result_round_trip_ms` 与是否精确关联。方法名 `native_send_start_to_vehicle_result_received`，包含 write、去程、车端服务处理、回程和云捎带等待；没有物理 CAN 执行确认。未测量值为 null，结果年龄单独报告。该诊断不改变既有 ACK recovery 或车端安全判断。
3. 页面 metrics 新增 `network_path`、`media_transport_rtt_ms`、`control_rtt_source` 和 `ice_connectivity`。保留 `control_rtt_ms` 兼容字段并明确其来源是媒体 ICE。候选对明细最多16条，汇总覆盖全部 pairs，缺 counters 与 0响应分别保留；失败/已尝试的直接 checks 优先保留。没有输出地址、SDP、ICE凭据。
4. `LowestLatencyPathPolicy` 是**未接入生产发送的建议模块**，不能授予控制权、改 ICE nomination 或实际切路。只比较配置指定的同一种端到端度量；拒绝把云入口 ACK/ICE RTT用作控制结果。拒绝结果仅作诊断；上层健康资格必须要求足够的、通过原有门禁的neutral/有效结果，不能因拒绝响应快就允许驾驶。当前 session及transport attempt、明确可选择、健康、足够样本、时效、有限非负值均须成立。绝对/相对改善、连续稳定、新样本推进、最小驻留抑制抖动；当前路径不健康时可立即建议健康回退；无健康路径返回 none。只有调用方确认实际切换完成后才 `confirm_selected`；确认需匹配session generation和非零attempt，旧确认被拒绝，新attempt重新计稳定窗口。

策略构造必须显式提供所有预算；测试中的200ms/3样本/5ms/10%/100ms/300ms只是 fixture 数值，**不是产品默认、批准阈值或安全阈值替换**。本提交没有修改任何安全阈值、云/台架配置或网络参数。

## 后续最小可实施设计（尚未实现，需主线程协调语义）

1. **三方能力协商。** 云、Mac、车端声明独立版本化 transport/result capability，绑定 session generation、driver/vehicle、media attempt；不凭旧v9能力串或DC open乐观启用普通控制。旧端维持现有 WSS。
2. **唯一 native 生产源和受限本机桥。** native仍分配全局seq/时间、管理intent lease/ESTOP；浏览器仅转送原封已生成的最新包。桥绑定 session+generation+ui owner/epoch+media attempt+transport epoch，队列只留一个最新 ordinary 包，过期或页面停顿后不补发、不重打时间戳。仍保留云认证、排他租约和撤销；车端消费已签发到期时间做有界授权，不默认离线驾驶。
3. **单一路径和共同接收器。** DC/WSS普通包进同一个 mutex、序号/时效/安全服务。`transport_select` 使用严格版本协议和CAS epoch栅栏，绑定 from_epoch、next_epoch、path、min_command_seq。先撤回运动、退休旧桥队列，车端确认后在新路径发安全 neutral 样本验证结果，再由既有healthy+松开重按恢复运动。旧epoch高seq包仍拒绝；会话中序号不归零。pending/超时保持制动；不可因DC open自动恢复旧按键。
4. **同语义车端结果。** 接收结果包含scope/epoch/path、command_seq/intent_seq、result_seq、accepted/reason/rearm/status_seq。WSS独立推送结果，不等下一入口ACK；DC回传相同结果并经本机桥交native。明确软件接受和物理CAN反馈分开。低序号缺结果不因累计高seq而被宣称接受。重复包只允许有界缓存返回原结果，不再次apply、不刷新watchdog。
5. **分离控制服务与单个DC生命周期。** 仅在新协商模式下让同一receiver/adapter活到session结束；单DC断开仍撤回运动/请求制动。补WSS profile/VCU/遥测/安全退出管理路径，否则不能称完整保底。视频/clock/profile/VCU/readiness门禁全部保留。
6. **最快健康路径而非固定P2P优先。** 备用通道可发带scope+nonce的非执行探测；probe RTT单独命名，不能刷新控制watchdog，也不能替代命令结果。先探测排名，再通过受栅栏的neutral验证取得真实结果样本；ordinary不双发。记录 `(command_path, feedback_path)`，避免把非对称回程当单条去程延迟。反馈一向时延需可信时钟与不确定度；否则只比较可证明的round trip。
7. **媒体由ICE负责连通。** 单PC `all` 暴露候选并让ICE选可用路径；它不承诺按capture→presentation最小值提名。浏览器没有可移植 API 按应用分数随意强制已检查的pair。不能把非选中pair的低ICE RTT标成可选择/最低视频端到端延迟；先闭合候选缺口，必要时离线/协调A-B测量相同编码设置下的真实路径。任何ICE restart须限定当前attempt并撤回/恢复输入，不默认无感运动切换。

需主线程先确认的取舍：切换先制动和松开重按；WSS管理保底范围；是否信任当前受scope约束页面转报的车辆结果，或需native验证签名/MAC/终结RTC；ESTOP是否另做两条已授权路径同seq冗余。当前仍保持既有WSS急停语义，未自行开放DC或双路发送。

## 最小有界现场探测提案（未执行）

- 先读当前同一会话两端实际 `ice_transport_policy`、ICE服务器 scheme/transport计数、attempt标识、generated/sent/received/applied/rejected候选类型计数和失败分类；只存白名单，不导出SDP、地址或凭据。核实远端是否仍为relay策略，不能从Mac all推断两端all。
- 经主线程安排，当前PC每秒一次getStats、10次截止；记录selected证据等级、所有pair聚合与最多16条详情、RTP/帧/DC推进。已有采样缺历史，不能因晚采样没有pair认定早期从未尝试。
- 若仍缺生成/交换证据，在自有独立mock会话做一次最长30秒的all探测，限vehicle-001、camera6/7，无执行机构，不占用当前台架会话；使用既有签发服务器，凭据仅内存使用。TURN/UDP与TURN/TCP对照必须标记同一配置/会话范围和实际relayProtocol，不能把TCP Binding应答当allocation成功。
- UDP3478的有限重复探测只能报告该服务路径。新增STUN/TURN服务、端口、NAT/防火墙/VPN/路由修改属于另行网络变更，不在本任务执行。

## 验收矩阵

| 用例 | 预期 | 状态 |
| --- | --- | --- |
| 云ACK快、车端结果3702/3661ms；结果accepted/rejected | 精确seq/intent计时；不被云ACK清掉、不混同接受和物理执行 | 纯C++已覆盖 |
| 重复、倒序、未发送、evicted、intent不符、时钟倒退 | 不刷新结果进度；未知为null；固定128条 | 纯C++已覆盖 |
| P2P70ms、TURN/UDP40ms、TURN/TCP55ms、WSS20ms | 选健康可选择的WSS；WSS失效再选TURN/UDP | 策略fixture已覆盖，无实际切换 |
| ICE RTT/云入口ACK、旧scope、stale/future、样本不足、NaN、不可选择pair | 不作控制端到端健康证据 | 策略fixture已覆盖 |
| 较小改善、同样本重复读、改善抖动、最小驻留、active硬失效 | 保持/防抖；新样本稳定后才建议；硬失效不等驻留；none触发上层停车设计 | 策略fixture已覆盖 |
| relay候选protocol=udp但relayProtocol=tcp/未知；缺远端candidate | TCP/未知正确分类，不误报P2P/UDP | JS及实际页面方法mock已覆盖 |
| 超16个pairs，后出现失败direct | 完整聚合、失败direct详情优先、未知counter不同于0 | JS已覆盖 |
| 旧epoch高seq晚到、双路同seq重复/冲突、media attempt换代 | 共同receiver只执行一次；旧身份退休；不刷新watchdog | 后续协议/接收器实现验收，未实现 |
| DC→WSS→DC按键持续按住、切换ACK丢失、云撤销/租约过期 | 制动、旧按键不复活、授权fail closed、确认后松开重按 | 后续跨端验收，未执行 |
| 切换并发ESTOP、DC全断后管理/遥测/安全退出、无反馈而入口ACK健康 | 明确优先级/去重；保底不绕门禁；失联停车 | 需语义协调及现场验证 |
| 真实P2P、TURN/UDP、TURN/TCP、WSS最低延迟/回退 | 真实同端点样本、相同scope与健康预算、切换前后全程安全 | 未完成；离线通过不等于现场通过 |

复现：`node cpp/tests/ice_page.test.cjs`、`node cpp/tests/control_logic.test.cjs`；配置本地依赖后构建 `mine_teleop_runtime` 和 `mine-teleop-latency-path-tests`，仅运行该纯测试或 `ctest -R '^mine-teleop-latency-path-tests$'`。固定回归入口 `python3 scripts/test/run_offline_regressions.py` 不使用台架。具体提交、日志和结果见交接回执。
