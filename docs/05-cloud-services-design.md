# 云端服务设计

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

## 职责

云端服务负责连接协调、鉴权、会话管理、审计和上传目标管理。它不应该承载车辆安全控制的最终责任。

云端组件：

- Auth Service：用户和设备鉴权。
- Signaling Service：WebRTC 信令。
- Session Service：会话和控制权。
- TURN Service：NAT 穿透兜底。
- Upload API：录像上传凭证或上传登记。
- Audit Log：审计日志。

## 部署建议

实时服务与普通业务隔离。

建议：

- 固定 IP 云服务器保留为入口和管理节点。
- 新增支持 UDP 的轻量节点作为 STUN/TURN。
- TURN 节点尽量靠近车辆和驾驶端网络路径。
- 如果现有云服务器负载较高，不建议让 4 路视频长期全部经它中继。

## 鉴权

### 车端设备

推荐：

- 每台车一个设备 ID。
- 每台车一组设备证书或长期凭据。
- 车端启动后向云端注册在线状态。
- 云端校验设备身份后允许建立会话。

当前 C++ CLI 默认只注册一个 `vehicle_id`/`device_token` 开发组合；服务核心支持
多个独立车辆凭据和驾驶员到车辆的允许列表，但尚未实现持久化凭据文件或数据库。
管理员配置 `MINE_TELEOP_ADMIN_TOKEN` 后，可通过受保护 API 单独撤销或恢复一辆车，
撤销会立即使该车下线并关闭它的会话，不影响其他车辆。真实设备证书、凭据哈希、
轮换和持久化仍需在生产边界接入。

### 驾驶端用户

推荐：

- 用户登录获取短期令牌。
- 令牌用于信令和会话请求。
- 控制权只授予一个用户。

本地参考实现生成带 `expires_at_ms` 的短期 bearer token，默认有效期 30 分钟；
创建会话前会校验驾驶员 ID、token 归属、过期时间和该驾驶员的车辆允许列表。
过期、已撤销和权限不匹配的 token 都会被拒绝。相同账号仍在线时仅凭密码重复
登录返回 `409`。管理员可单独撤销或恢复一个驾驶员；撤销会删除该驾驶员 token
并关闭其会话，不影响其他驾驶员。当前密码仍是进程内明文开发配置，PBKDF2/Argon2
存储、外部 IAM、账号生命周期和轮换仍未实现。

本地 C++ 服务已实现登录失败防暴力破解：每个已配置驾驶员有独立的失败桶，所有
未知账号名共用一个固定桶，因此随机账号名不能让限流表无限增长。默认在 60 秒
窗口内第 5 次失败时锁定登录 300 秒；阈值失败及锁定期请求返回 HTTP `429`，
同时带 `Retry-After` header 和 JSON `retry_after_ms`。有效密码不能绕过仍在生效
的锁定；锁定到期后可以重新登录，成功后清除该账号的失败状态。失败和进入锁定
分别审计为 `driver_login_failed`、`driver_login_rate_limited`，不记录密码，未知
账号也不保留攻击者提交的原始 ID。三个参数由 `--login-max-failures`、
`--login-failure-window-ms`、`--login-lockout-ms` 配置，必须都是正数。
失败计数和锁定状态先于审计落盘更新；即使审计路径临时不可写，请求会失败关闭，
但已经达到阈值的账号仍保持锁定，不能靠制造审计故障绕过保护。
登录成功和错误响应都带 `Cache-Control: no-store`，避免短期 bearer token 或认证
状态被浏览器、中间代理缓存。

该机制按账号保护凭据，不具备可信来源 IP，因为回环后端看到的 socket 对端是
TLS 反向代理。公网仍必须在可信代理/负载均衡层补充按来源和路由的通用 API
限流，并验证代理头信任边界；不能用账号锁定替代 S9 的整套 API 限流门禁。
登录、车辆上线和会话请求中的 `driver_id`、`password` 与 `vehicle_id` 必须是
JSON string，不能用布尔值或数字让服务端隐式转换。
驾驶端短期 token、车端 `device_token` 和上传设备凭据同样必须是 JSON
string；缺失凭据按认证失败处理，布尔值或数字凭据按请求格式错误处理。

当前原生客户端已把后续 GET/WSS 鉴权凭据从 URL query 迁到
`X-Mine-Teleop-Driver-Token` 和 `X-Mine-Teleop-Device-Token` 请求头，避免 bearer
值进入反向代理 access log、浏览器/终端历史和 URL 错误信息。登录与车辆注册仍使用
TLS 保护的 POST JSON body。服务端暂时保留 query 读取作为旧客户端兼容回退，但新
客户端不会生成带 token 的 URL；完成所有端升级并轮换旧凭据后应删除该回退。

## 会话模型

首版一车一驾驶员。

会话状态：

- `offline`：车辆不在线。
- `online`：车辆在线但没有控制会话。
- `reserved`：服务端已为获权驾驶员预留车辆。
- `connecting`：参与者正在交换信令并建立媒体/控制链路。
- `active`：会话和短期控制权有效。
- `degraded`：会话仍存在，但媒体、控制或时间同步处于降级状态。
- `stopping`：控制权已撤销，服务端正在清理队列和参与者状态。
- `closed`：终态；旧 `control_token` 已清除且不可恢复。

当前同步 HTTP 创建流程会依次审计 `reserved → connecting → active`。显式结束、
控制权撤销或车辆离线会先清空控制令牌和待投递消息，再审计
`active → stopping → closed`。车辆重连只能创建带新令牌的新会话。

控制权规则：

- 同一时刻一车只有一个控制者。
- 控制权发放需要云端记录。
- 控制权回收也需要云端记录；本地参考实现提供
  `POST /sessions/{session_id}/control_authority/revoke`，要求当前会话参与者凭据，
  记录 `control_authority_revoked` 原因并释放车辆给后续会话。
- 车端也需要校验会话 ID 和令牌，不能只信驾驶端。
- 控制消息还必须携带并校验同一会话的 `driver_id`；不能只凭 `vehicle_id` 执行。

### 在线连接、心跳和代次

本地 C++ 服务用连接代次隔离同一车辆的旧进程：

- `POST /vehicles/online` 必须携带 `vehicle_id`、`device_token` 和本次车端
  supervisor 生成的 `connection_id`，返回正整数 `connection_generation`。
- 同一 `connection_id` 的重复上线是幂等刷新，返回
  `duplicate_policy=same_connection_refresh`；车端 supervisor 启动的控制和媒体
  子进程必须共享该 ID。
- 不同 `connection_id` 会创建新代次，返回
  `duplicate_policy=replace_previous_connection`，并关闭该车旧会话、清空旧控制
  token 和信令队列。
- 车端会话发现、信令读取/发送、ICE 获取、显式心跳和下线都必须携带当前
  `connection_generation`；旧代次返回 `409`，不能刷新新连接或读取新会话。
- `POST /vehicles/heartbeat` 可显式刷新在线时间；正常运行时，经过认证的会话
  查询和信令流量也会刷新在线时间。默认 15 秒未出现心跳后转为 offline，关闭
  活动会话并记录 `vehicle_offline(reason=heartbeat_timeout)`。

驾驶端登录返回独立的连接代次。相同账号仍在线时，第二次仅凭密码登录返回
`409`，不能替换活动连接。当前控制台必须持现有 token 调用
`POST /auth/driver_logout` 才能释放会话和登录状态；也可调用
`POST /auth/driver_heartbeat`。浏览器 `pagehide` 会通过 loopback C++ 进程触发
logout；异常关闭则由默认 15 秒驾驶端心跳超时回收控制权。

控制权 token 使用独立于驾驶员登录 token 的短 TTL，响应包含
`control_token_expires_at_utc_ms`。当前驾驶员可用登录 token 调用
`POST /sessions/{session_id}/renew`；服务端只允许会话中的驾驶员续租，拒绝车端代续，
保持原 session 和 `control_token`，只把到期时间向后延长并记录不含 token 的
`control_authority_renewed`。控制端在租期约 1/3 处开始续租，为浏览器调度抖动保留
约 2/3 租期；如果控制端停止刷新，续租不会在服务端自行发生。到期时服务端执行
`active/degraded → stopping → closed`、立即清除 token，并审计
`control_authority_expired`。控制台重新连接前会通过
`GET /sessions/{session_id}` 校验本地会话；已被回收的会话不会在本地静默复用。

## 信令

信令服务建议使用 WebSocket。

受控 HTTP API 生命周期事件：

- `vehicle_online`
- `vehicle_offline`
- `driver_login`
- `session_request`
- `session_accept`
- `session_reject`

客户端可入队信令消息：

- `webrtc_offer`
- `webrtc_answer`
- `ice_candidate`
- `session_end`

控制权回收是 server-side 状态变更，应通过受控 API 触发并写审计，不允许普通
信令消息伪造 `control_authority_revoked`。
本地参考实现中，`vehicle_online`、`vehicle_offline`、`driver_login` 和
`session_request` 由 HTTP API 处理；`session_accept`/`session_reject` 属于
后续显式会话协商模型的 server-side 生命周期结果，不能通过普通信令消息伪造 `session_accept`、`session_reject` 或控制权状态变更。

信令只负责建立连接和会话状态，不转发每条控制命令。

本地参考实现对信令消息发送方、收件人、消息拉取方和 WebSocket 升级参与者
都执行会话参与者校验；驾驶员侧必须带短期登录 token，车端侧必须带设备
token。非当前驾驶员或车辆不能写入、读取、连接该会话的信令通道，也不能
作为信令消息收件人被排队。
信令消息的 `type`、`sender` 和 `recipient` 必须是 JSON string，`payload`
必须是 JSON object；不能通过布尔值、数字或可转 dict 的数组让服务端隐式转换。
WebSocket 连接中的非法 JSON 消息或非对象消息必须返回结构化 `error` 帧并停止
处理该连接，不能抛出服务端异常或把非法消息排队。
WebSocket 握手必须显式带 `Upgrade: websocket`、`Connection: Upgrade`、
`Sec-WebSocket-Version: 13` 和合法的 16-byte base64 `Sec-WebSocket-Key`；
缺少或伪造这些头时必须返回 400 JSON error，不能把普通 HTTP GET 误升级。
客户端发往服务端的 WebSocket 帧必须按协议 masked；未 masked 的客户端消息必须
返回结构化 `error`，并且不能进入信令队列。
本地最小 WebSocket 信令实现只接受完整单帧 text message；fragmented message
必须返回结构化 `error`，不能把首帧当成完整信令消息排队。
WebSocket control frame 必须遵守协议长度限制；payload 超过 125 bytes 的
close/ping/pong frame 必须返回结构化 `error`，不能被当作正常关闭或普通消息。
所有 payload length 必须使用 RFC 6455 的最短编码；保留位、保留 opcode、超过
信令上限但未发送 body 的长度头也必须在分配 payload 内存前拒绝。close payload
只能为空或包含合法状态码，reason 必须是完整 UTF-8。当前原生 TCP 门禁还会并发
建立 32 条已认证连接，逐条完成 masked ping/pong 与合法 close echo，确保畸形帧
拒绝不会破坏随后正常连接的 control frame 处理。

当前独立 C++ `mine-teleop-signaling-server` 默认只绑定 `127.0.0.1`，在同一
回环监听器上提供 HTTP API 和 `/signaling/{session_id}/ws` WebSocket。服务端会
把车端经受控 API 入队的 offer/ICE/status 主动推送给已认证控制端，控制端的
answer/ICE 则经 WebSocket 写回同一套参与者隔离队列。WebSocket 在握手后仍会
持续复核 token、会话和控制权；控制权到期时发送错误/close 并停止投递。
WebSocket push 不会在 socket 写出前删除队列消息。每条消息带服务端分配的
`delivery_cursor`，push envelope 带当前最高游标；客户端保存消息后发送
`signaling_delivery_ack`，服务端只清理不高于已确认游标的消息。连接在确认前
中断时，同一有效会话重连会补发，客户端按游标去重；确认游标不得超过该连接
实际发送过的最高值。

客户端到服务端的信令 ACK 包含原 `seq`、稳定 `message_id` 和目标队列的
`delivery_cursor`。相同会话、发送方、序列号和内容的重试返回原确认并标记
`duplicate=true`，不重复入队；同序列号改写内容仍返回 409。临时 HTTPS/WSS
不可达只把本地会话标为待验证，不能当作控制权已失效；服务器明确返回
401/403/404/409，或心跳/短期控制 token 已过期时，才清除本地控制权。

进程不内建 TLS。生产部署必须继续让它只监听回环地址，由
`deployments/caddy/Caddyfile`、nginx 或云负载均衡在 443 终止可信 TLS；同一
HTTPS 入口转发 API 和 WebSocket Upgrade，且其它路径统一 404，不能把明文
后端或控制页面直接暴露到公网。`Caddyfile.local-wss` 的内部 CA 只用于本机
验收，不能替代公网可信证书。

本地参考实现提供 `/sessions/{session_id}/ice_servers`：当前会话驾驶端可带
短期登录 token、车端可带设备 token 获取配置中的 STUN/TURN server 列表。
返回格式贴近 WebRTC `RTCIceServer`，包含 `urls`、TURN `username` 和
`credential`；服务只审计 `ice_servers_issued` 的数量和 TURN 数量，不把
TURN 密码写入审计日志。服务端用 `--stun-urls`、`--turn-urls`、
`--turn-realm` 和 `--turn-static-auth-secret-file` 配置同一组 ICE 端点；浏览器
和 GStreamer 车端都从该会话 API 获取配置。TURN server 启用时，服务会按
coturn REST API 约定生成
`<expires_unix>:<realm>:<session_id>:<actor>` 形式的用户名，并用
`static_auth_secret` HMAC-SHA1 签出短期 credential，同时返回
`expires_at_utc_ms`。真实 TURN 可达性、凭据轮换策略和 NAT 路径仍需在部署环境
验证。

会话结束、异常断开上报和实时诊断上报同样要求当前会话参与者凭据，避免仅凭
`actor` 字符串撤销控制权或写入审计事件。`actor`、异常断开 `reason` 和
`detected_by` 必须是 JSON string，不能用布尔值或数字让服务端隐式转换。

本地参考实现提供 `/sessions/{session_id}/diagnostics`：当前会话参与者可上报
`component`、RTT、丢包率、抖动、视频延迟和控制发送频率，服务校验对应驾驶员
token 或车端设备 token 后写入 `realtime_diagnostics` 审计事件。`component`
必须是 JSON string；RTT、抖动和视频延迟必须是非负整数 JSON number；丢包率
和控制发送频率必须是非负 JSON number，不能用字符串或布尔值代替。

本地参考实现还提供 `/sessions/{session_id}/control_timeout`：车端可带设备 token
上报最后有效控制接收时间、进入超时制动时间和配置阈值，服务写入
`control_timeout` 审计事件，供云端追溯控制链路超时。这三个毫秒字段必须是
非负整数 JSON number，且进入超时制动时间不能早于最后有效控制接收时间。

控制端把浏览器采集的 WebRTC 状态变化通过
`/sessions/{session_id}/webrtc_connection` 上报；服务严格校验
`connection_state`、`direct/STUN/TURN/unknown` 路径、TURN 一致性和时间同步字段。
连接成功、失败和其它状态分别审计为 `webrtc_connection_succeeded`、
`webrtc_connection_failed`、`webrtc_connection_state`；时间同步不可信时另写
`time_sync_anomaly`。控制运行时按 session + 状态 + 路径 + 时间可信度去重，
相同的周期 metrics 不重复刷审计。

## TURN

TURN 用于 P2P 失败或网络不稳定时兜底。

要求：

- 支持 UDP。
- 配置长期凭据或临时凭据。
- 日志记录中继流量。
- 对不同车辆或会话做带宽统计。

本地参考实现提供 `/sessions/{session_id}/turn_usage`：当前会话参与者可上报
`bytes_sent`、`bytes_received` 和 `duration_ms`，服务按会话累加
`relay_bytes_total`、计算最近样本 `last_bitrate_kbps`，并写入
`turn_relay_usage` 审计事件。每个参与者携带从 1 开始单调递增的 `sample_seq`；
完全相同的重试幂等返回，复用序列号改写内容或倒退序列会拒绝，避免网络重试重复
记账。三个用量字段必须是非负整数 JSON number，且 `duration_ms` 必须大于 0。
审计写入成功后才提交内存累计。TURN relay 启用和用量上报都必须带当前
会话参与者凭据；累计值同时出现在会话响应和 `/health` 的
`turn_usage_sessions`/`turn_relay_bytes_total`，整数溢出会失败关闭。本地参考实现
还提供 coturn `usage` 日志解析边界：约定
`username=<session_id:actor>` 或 REST 临时凭据形态
`username=<expires:realm:session_id:actor>`，解析 client `usage` 与 `peer usage`
中的 `rp`、`rb`、`sp`、`sb` 后，可通过内部可信 ingest 写入同一套
`turn_relay_usage` 汇总和审计。coturn 4.6.2 的这些结束记录不含可靠的会话时长，
因此不能仅凭原始日志推导平均带宽；带宽仍应使用参与者上报的 `duration_ms` 或
外部指标采样计算。`scripts/coturn_usage_report.sh` 可从 coturn 日志输出脱敏
JSONL 验收报告，记录解析样本数、会话数、TURN client bytes 与实际 peer relay
payload bytes，不回显原始 username。云端带宽账单对账仍需在部署环境中接入和验证。

注意：

- TURN 会增加延迟和云端流量成本。
- TURN 节点不要和高负载业务混用。

## 录像上传

默认设计为对象存储/S3 兼容。

两种上传模式：

1. 车端从云端获取预签名上传 URL，直接上传对象存储。
2. 车端上传到云端 Upload API，由后端转存对象存储。

推荐第一种：

- 云服务器不承载大文件转发。
- 扩展性更好。
- 可以按会话/车辆/日期组织对象路径。

预签名 URL 必须有续签流程：

- 云端下发 URL 时同时返回过期时间和对象路径。
- 车端上传前检查 URL 剩余有效期，低于安全余量时重新申请。
- 上传重试、断电重启或队列恢复后，不复用已过期 URL。
- 云端应允许对同一片段重新签发相同对象路径，避免重复对象。
- 上传凭证签发阶段必须校验车辆、会话、相机和片段 ID 是安全对象路径段，
  不能包含路径分隔符或 `.`/`..`。
- 当前 C++ 运行时只实现原子 `LocalArchiveUploader`，用于开发/离线归档；独立
  信令服务没有 Upload API，也没有 S3-compatible SigV4 预签名或上传完成登记。
- 对象存储直传、短期 URL 续签、跨车辆路径校验、视频/sidecar 双对象登记、凭据
  轮换和厂商兼容性仍属于未实现的服务端/车端工作，不能沿用已移除 Python 参考栈
  的旧结论。

对象路径建议：

```text
vehicles/{vehicle_id}/sessions/{session_id}/cameras/{camera_id}/{start_ts}.mp4
vehicles/{vehicle_id}/sessions/{session_id}/metadata/{segment_id}.json
```

## 审计日志

云端至少记录：

- 用户登录。
- 车辆上线/下线；下线导致活跃会话失效时，应同时记录受影响会话。下线
  `reason` 必须是 JSON string。
- 会话创建/结束。
- 控制权授予/回收。
- 急停；本地信令参考服务暴露会话级 `estop` 审计入口，要求当前会话参与者凭据，并记录触发原因和非负整数 JSON number 控制序号。
- 异常断开，至少包含会话、参与者、原因和检测组件。
- 控制链路超时；本地信令参考服务暴露会话级 `control_timeout` 审计入口，要求当前会话参与者凭据。
- TURN 中继启用，至少包含 TURN URL、relay candidate 和 selected pair 等 JSON string 诊断字段。
- WebRTC 成功/失败、Direct/STUN/TURN 路径和时间同步异常。
- ICE server 发放，至少包含发放对象、ICE server 数量和 TURN server 数量，不能记录 TURN credential 明文。
- 上传成功/失败。

C++ 信令服务会在统一审计写入边界递归脱敏任何 key 中含 password、token、
secret、credential、authorization、cookie 或 private/API key 的字段；这同样覆盖
`diagnostics`、`turn_usage`、`control_timeout` 等认证后报告，不能只依赖调用方删除
凭据。上述报告路由还会先做字段白名单和严格类型/范围校验，只把会话关联字段和
指标写入审计：字符串数字、布尔数字、超过 100% 的丢包、倒序制动时间、驾驶端
伪造车端 timeout、零时长 TURN 样本都会拒绝。全部审计写入、按大小轮转、按小时
归档和保留期清理都由独立 mutex 串行化，避免并发请求同时操作同一个文件导致崩溃。
当前文件为 `signaling-audit.jsonl`；每个 UTC 小时结束后改名为
`signaling-audit.YYYYMMDDTHHMMSSZ.partNN.jsonl`。默认保留最近 7 天，
`--audit-log-retention-days` 可在 1 到 365 天间调整；`--audit-log-max-bytes` 和
`--audit-log-files` 仍限制当前小时内的大小分片。部署仍需外部日志采集平台完成
持久查询、告警和磁盘压力验收。
