# 云端服务设计

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

本地参考实现默认只注册 `vehicle-001` 的开发 token；联调或部署时可通过
`signaling-server --device-credentials` 加载车辆凭据文件，显式声明允许上线和
使用 Upload API 的车辆 ID 与 `device_token`。配置该文件后，文件内车辆必须使用
对应 token，不能继续用默认开发 token 通过认证。真实设备证书、吊销和轮换仍需在
生产环境接入。

### 驾驶端用户

推荐：

- 用户登录获取短期令牌。
- 令牌用于信令和会话请求。
- 控制权只授予一个用户。

本地参考实现的 `DriverTokenStore` 生成带 `expires_at_ms` 的短期 bearer
token，默认有效期 30 分钟；创建会话前会校验驾驶员 ID、token 归属和
过期时间，过期 token 会被拒绝。未显式配置凭据文件时保留 `dev-password`
作为本机开发默认值；联调或部署时可通过 `signaling-server --driver-credentials`
加载 PBKDF2-SHA256 驾驶员凭据文件，让默认开发密码对已配置驾驶员失效。外部
IAM、账号生命周期和凭据轮换策略仍需在生产环境接入。
登录、车辆上线和会话请求中的 `driver_id`、`password` 与 `vehicle_id` 必须是
JSON string，不能用布尔值或数字让服务端隐式转换。
驾驶端短期 token、车端 `device_token` 和上传设备凭据同样必须是 JSON
string；缺失凭据按认证失败处理，布尔值或数字凭据按请求格式错误处理。

## 会话模型

首版一车一驾驶员。

会话状态：

- `IDLE`：车辆在线但无人控制。
- `REQUESTED`：驾驶端请求连接。
- `SESSION_ACTIVE`：会话已建立，控制权有效。
- `ENDING`：会话结束中。
- `ENDED`：会话结束。
- `FAILED`：会话失败。

控制权规则：

- 同一时刻一车只有一个控制者。
- 控制权发放需要云端记录。
- 控制权回收也需要云端记录；本地参考实现提供
  `POST /sessions/{session_id}/control_authority/revoke`，要求当前会话参与者凭据，
  记录 `control_authority_revoked` 原因并释放车辆给后续会话。
- 车端也需要校验会话 ID 和令牌，不能只信驾驶端。

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

`signaling-server --serve` 默认只绑定 `127.0.0.1`，允许本机开发使用明文
HTTP；如果显式绑定非回环地址，启动参数必须同时提供 `--tls-cert` 和
`--tls-key`，服务会以 HTTPS 方式监听。生产部署也可以让该进程继续只监听
回环地址，再由 nginx/Caddy/云负载均衡终止 TLS，但不能把明文 HTTP 直接暴露到
公网。

本地参考实现提供 `/sessions/{session_id}/ice_servers`：当前会话驾驶端可带
短期登录 token、车端可带设备 token 获取配置中的 STUN/TURN server 列表。
返回格式贴近 WebRTC `RTCIceServer`，包含 `urls`、TURN `username` 和
`credential`；服务只审计 `ice_servers_issued` 的数量和 TURN 数量，不把
TURN 密码写入审计日志。`signaling-server --vehicle-config` 会读取车端配置中的
`ice` 段来提供该端点。若 TURN server 配置为 `credential_mode: turn_rest`，
服务会按 coturn REST API 约定生成
`<expires_unix>:<username>:<session_id>:<actor>` 形式的用户名，并用
`static_auth_secret` HMAC-SHA1 签出短期 credential，同时返回 `expires_at_ms`。
真实 TURN 可达性、凭据轮换策略和 NAT 路径仍需在部署环境验证。

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
`turn_relay_usage` 审计事件。三个用量字段必须是非负整数 JSON number，
且 `duration_ms` 必须大于 0。TURN relay 启用和用量上报都必须带当前
会话参与者凭据。本地参考实现还提供 coturn `usage` 日志解析边界：约定
`username=<session_id:actor>` 或 REST 临时凭据形态
`username=<expires:realm:session_id:actor>`，解析 `rb`、`sb` 和 `duration_ms`
后可通过内部可信 ingest 写入同一套 `turn_relay_usage` 汇总和审计。
`scripts/coturn_usage_report.py` 可从 coturn 日志输出脱敏 JSONL 验收报告，
记录解析样本数、忽略行数、会话数、relay bytes 累计量和平均带宽，不回显原始
username。云端带宽账单对账仍需在部署环境中接入和验证。

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
- 本地参考实现支持 S3-compatible SigV4 预签名 PUT URL 生成；Upload API 服务可注入
  configured `UploadCredentialService`/S3 signer，以使用真实 endpoint、bucket、region
  和凭据签发 URL。目标对象存储厂商的签名兼容性、凭据轮换和权限策略仍需在部署
  环境中验证。
- 车端 `VehicleRecorderUploader.from_config()` 在 `upload.backend=s3` 时使用
  direct HTTP PUT uploader，把视频和 sidecar 分别上传到 Upload API 签发的
  `upload_url`/`metadata_upload_url`，上传成功后再登记两个对象；本地
  `local_archive` 后端仍保留为开发和离线测试用途。
- `signaling-server` 开发入口支持通过 `--vehicle-config` 读取 `upload.backend=s3`
  和 `upload.s3` 目标配置来构造签名服务；有效配置日志必须脱敏 access key、
  secret、session token 和 secret 文件路径。
- `scripts/upload_presign_report.py` 可离线读取同一份车端配置，为 video 和 metadata
  各签发一条样例凭证，并输出脱敏 JSONL 验收报告。报告只记录 endpoint、bucket、
  region、对象路径、签名算法、过期时间和 signature/session-token 是否存在，不输出
  完整 URL、access key、secret 或 session token。真实对象存储 PUT、权限策略和厂商
  兼容性仍需在部署环境执行。
- 本地 Upload API 在登记上传成功/失败时，会从标准对象路径恢复
  `vehicle_id` 和 `session_id` 写入审计记录，避免归档事件丢失会话归属。
- 如果请求 payload 的 `vehicle_id` 与对象路径中的车辆 ID 不一致，服务按对象路径
  归属校验设备 token 并拒绝跨车辆登记。
- 上传成功/失败登记只接受上述 `cameras/*.mp4` 或 `metadata/*.json` 标准对象路径；
  非标准路径会被拒绝，避免审计记录失去车辆和会话归属。
- 上传成功/失败登记中的 `segment_id` 必须与标准对象路径末尾片段一致，避免审计
  中出现对象路径和片段 ID 指向不同片段。
- 上传成功/失败登记的 `segment_id`、`object_path` 和失败 `error` 必须是 JSON
  string，不能用布尔值或数字让服务端隐式转换。
- 上传成功登记的 `bytes_uploaded` 必须是非负整数 JSON number，不能用字符串或布尔值代替。

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
- ICE server 发放，至少包含发放对象、ICE server 数量和 TURN server 数量，不能记录 TURN credential 明文。
- 上传成功/失败。

本地参考实现的 `AuditLog` 写入 JSONL，并可按大小轮转为 `.1`、`.2` 等编号
备份文件；`signaling-server --serve` 可通过 `--audit-log-max-bytes` 和
`--audit-log-backup-count` 配置轮转，避免长期运行时审计文件无限增长。
