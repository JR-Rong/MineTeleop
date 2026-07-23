# macOS 控制端构建与验收

当前已实测平台为 macOS arm64；x64 支持交叉构建但仍需 Intel Mac 或 Rosetta
运行验收。控制端使用 C++20 回环服务和系统浏览器，
不编译车端的 GStreamer、JPEG、相机或录像模块。

## 构建压缩包

```bash
scripts/build_macos_control_bundle.sh
```

脚本会执行以下门禁：

1. 固定提交的 `yaml-cpp` 与 `nlohmann-json` 依赖配置；
2. `mine-teleop-control` 和控制端测试原生编译；
3. CTest 协议向量、回环 HTTP、端口冲突和页面安全契约；
4. 使用 macOS Security/CommonCrypto 系统接口，不携带 OpenSSL；
5. 对可执行文件进行 ad-hoc 签名；
6. 检查 Mach-O 不引用 `/opt/homebrew`、`/usr/local` 或 OpenSSL；
7. 写入目标架构和测试状态 `BUILD-INFO.txt`，生成 `tar.gz` 与 SHA-256 文件；
8. 对最终压缩包执行 `scripts/check_macos_control_bundle.sh`；所有架构检查校验和、
   签名、架构、依赖和内容，原生架构再验证解压启动、回环监听、占用端口报错、
   页面脚本、本地日志脱敏和退出后端口释放。

输出位于：

```text
dist/mine-teleop-control-macos-arm64-YYYYMMDD-HHMMSS.tar.gz
dist/mine-teleop-control-macos-arm64-YYYYMMDD-HHMMSS.tar.gz.sha256
```

当前 WSS 游标补发、WebSocket 畸形帧拒绝、代理重连、登录失败锁定、HTTP/WSS
来源限流、请求关联 ID、审计有界轮转与递归凭据脱敏、WebRTC 状态/路径审计、
TURN 用量幂等累计、失败切车保权、token 过期重登、非阻塞时间重同步，以及
Ubuntu 车端双摄/DataChannel 现场联通后的成品：

- `mine-teleop-control-macos-arm64-20260723-064607.tar.gz`，SHA-256
  `6d6f610b7d435383bf9872e745aafafc26539f89d2cb5b430d8fb8c33ad9c38d`，
  642,150 bytes，包含应用内固定解析、现场根 CA、三机现场配置、鉴权 header 迁移
  和云端重启恢复语义修复，3/3 原生测试及成品运行门禁通过；包内 README 已明确
  现场路径直接连云端，不需要 SSH/SOCKS/FRP；
- `mine-teleop-control-macos-x64-20260721-232651.tar.gz`，SHA-256
  `6f93f14513f6e87b13725a516f9b14bbe0bc38d77ffd831ef4dafecdf6bdc6c0`，
  649,972 bytes（`du` 636 KiB），仅交叉编译静态门禁通过，明确标记 build-only。

也可以对已有包单独重复成品验收：

```bash
scripts/check_macos_control_bundle.sh \
  dist/mine-teleop-control-macos-arm64-YYYYMMDD-HHMMSS.tar.gz
```

带 `runtime_tests_executed=no` 的交叉编译包只执行签名、架构、依赖和内容检查，
不会伪装成目标架构运行验收。

## 解压启动

```bash
tar -xzf mine-teleop-control-macos-arm64-*.tar.gz
cd mine-teleop-control-macos-arm64-*
./run-control.command
```

默认行为：

- 只监听 `127.0.0.1:8080`；
- 自动打开系统默认浏览器；
- 从 `config/driver-console.yaml` 读取与其他平台一致的配置；
- 另附 `config/driver-console.three-machine.yaml`，供当前云端三机现场路径显式选择；
- 三机配置通过应用内 `cloud.resolve` 把 `teleop-field.internal:6000` 指向云服务器，
  并用同目录现场根 CA 完整校验 TLS；不会修改系统 hosts，也不依赖代理；
- 从 `certs/cacert.pem` 提供 CA bundle；
- 把页面 UTC 事件写入包根目录 `.local/logs/control-browser-events.jsonl`，默认
  单文件 2 MiB、保留当前文件及两个编号备份，凭据类字段写盘前脱敏；
- `Ctrl-C` 后关闭回环端口并尝试释放活动控制会话。

### `all` 与强制 TURN

`cloud.ice_transport_policy: all` 是正常运行模式：客户端收集 host、STUN
反射和 TURN relay 候选，由 ICE 选择可达且优先级最高的路径，通常先尝试低时延
直连/STUN，失败时自动回退 TURN。`relay` 是验收/受限网络模式，只允许 TURN
relay 候选，用来证明云端中继路径确实可用。两者都使用 DTLS-SRTP 和加密
DataChannel；`relay` 通常增加云端带宽费用和往返时延。

控制端登录仍是 HTTPS POST；登录后的 bearer 通过
`X-Mine-Teleop-Driver-Token` HTTP/WSS header 发送，不再出现在 URL query。
服务端暂时保留 query fallback 兼容旧客户端。

轻量包使用系统浏览器，支持范围为 Safari、Chrome/Edge、Firefox 的当前与前一个
稳定主版本；Firefox 以 H.264 为必测兼容路径。现场验收必须记录精确浏览器版本，
H.265 始终按浏览器运行时能力探测，不能仅凭浏览器名称假定可用。

开发联调时可以覆盖服务地址和端口：

```bash
MINE_TELEOP_DRIVER_PASSWORD=dev-password \
  ./run-control.command \
  --port 28080 \
  --signaling-url http://127.0.0.1:8765 \
  --no-open-browser
```

## 已实测证据

- 架构：Mach-O 64-bit arm64；
- 外部动态依赖仅为 macOS 系统库和系统 framework；
- 实际监听为 `TCP 127.0.0.1:<port>`，使用 Mac 局域网地址访问得到
  `Connection refused`；
- 第二进程占用同一端口时显示 `Address already in use`；
- 浏览器首屏、连接、等待车端媒体、断链控制告警和离页释放均通过；
- 从 arm64 成品启动后在 Codex 内置浏览器实测：错误密码得到 401 且输入框
  立即清空；正确登录后识别在线授权车辆、建立 `session-000001`、获得控制权并正确
  停在“等待车端媒体”；安全退出后服务端 `active_sessions=0`、
  `online_drivers=0`，浏览器控制台无 warning/error；
- 同一真实浏览器流程验证页面急停后显示红色锁存提示、控制链路中断和“必须在车辆
  本地确认复位”，随后安全退出仍保留急停告警语义；本地 JSONL 记录登录、建会话、
  `control_estop_latched`、critical 监控状态和安全退出，且服务端会话已释放；
- 短 TTL 真实 Chromium 验收确认 `session-000001` 的驾驶员 token 到期后，页面自动
  回到登录界面，服务端 `active_sessions=0`、`online_drivers=0`；同一密码随后重新登录
  并建立 `session-000002`，审计记录 `reason=token_expired`。原生测试还固定了上游
  401 不会被本地代理误改为 409、到期 presence 清理和同一运行时重新登录；
- 双车原生测试确认有效切换在创建新会话前释放旧会话，切换后的控制命令只指向
  新车辆；新增两驾驶员、两车辆、两条同时在线 WSS 会话测试，证明忙碌或越权目标
  会在释放前被拒绝，原会话、控制命令和 WSS 均保持有效，双向 offer/answer 不串线，
  两个控制令牌相互独立且不进入审计；结束会话保留驾驶员登录，退出请求失败时保留
  可重试状态且不误报“已安全退出”；
- 真实 Chromium 页面在 `session-000001` 上尝试越权切换后显示“当前会话已保留”，
  页面信令轮询继续运行；本地运行时仍报告相同会话且 WSS connected，服务端保持
  `active_sessions=1`、`online_drivers=1`，随后页面安全退出将两者归零；
- 使用 `signaling-server.browser-switch.dev.yaml` 的真实 Chromium 双驾驶员场景先让
  `driver-console-002` 占用 `vehicle-002`，页面正确显示禁用的“控制中”；释放后，
  已连接 `vehicle-001/session-000002` 的页面在 5 秒刷新周期内将目标更新为“在线可控”，
  随后成功切换到 `vehicle-002/session-000003`。前后 WSS 都保持 connected，服务端
  始终只有 1 个活动会话，安全退出后会话/驾驶员归零，整个浏览器流程为 0 console
  error、0 warning，审计包含两条会话关联且不含测试凭据；
- 切车与退出会主动取消上一条浏览器信令长轮询，避免旧会话关闭后产生无害但误导的
  409 console error；忙碌或越权拒绝仍会在保留 Peer/DataChannel 的同时重启轮询；
- `scripts/check_macos_control_2x2.sh <build-dir>` 已在本机实际运行通过：一个 YAML
  多身份信令进程、两个原生控制进程、两辆模拟在线车同时达到 `2/2/2`，两条控制命令
  指向各自会话，双向越权请求返回 400 且原 WSS/权限不变，最终安全释放为 `2/0/0`；
- 同一脚本现可通过 `MINE_TELEOP_SOAK_SECONDS=1800` 切换到 30 分钟稳定性门，并用
  `MINE_TELEOP_KEEP_EVIDENCE=1` 保留 CSV、审计和进程日志。本机实际 1800 秒运行中
  两车/两驾驶员/两活动会话始终保持 `2/2/2`，完成 86 次控制权续期；350 组样本的
  早期/末段总 RSS 均值为 22,179/21,446 KiB（下降 733 KiB），总 FD 为 34→34，
  日志无测试凭据或 fatal/sanitizer 错误。双控制端安全退出后服务端为
  `active_sessions=0`、`online_drivers=0`、`status=ok`；
- 两个真实 Chromium 页面在同一 Mac 上组成浏览器控制端和浏览器车端模拟器，使用
  原生 `RTCPeerConnection`、两路 640x360/30 FPS Canvas 视频轨和参数严格为
  `label=control`、`protocol=mine-teleop-control-v1`、`ordered=false`、
  `maxRetransmits=0` 的 DataChannel。跨越 30 秒时间重同步点的 42 秒采样收到 840 条
  连续命令（seq 569..1408），平均 19.9976 Hz、最大间隔 61 ms、P95 间隔 58 ms；
  接收减发送时间 P50/P95/最大值为 1/2/3 ms，负时延、身份错误和序列断点均为 0；
  页面同时显示 front/rear 为 30/27 FPS、0% 丢包、Direct、约 12.9/14.5 ms，正常
  采样阶段控制台为 0 error、0 warning；
- 车端模拟器主动关闭 DataChannel 后命令数立即停止增长，控制页显示“控制链路中断”
  并把输入归零；独立新会话随后完成安全退出，服务端回到 0 活跃会话/0 在线驾驶员，
  控制页全程 0 error、0 warning；
- 为消除旧实现重同步造成的约 67 ms 未来时间戳和 265 ms 控制发送停顿，时钟现在允许
  使用新的服务端 UTC 锚点向后校正，周期同步由 20 Hz 发送热路径移到状态刷新路径；
  新原生回归测试覆盖正偏移后的向后校正，真实浏览器结果固定最大控制间隔为 61 ms；
- 控制端现用当前驾驶员 token 在短期控制租约约 1/3 处续租，只延长到期时间而不轮换
  session 或 DataChannel `control_token`。140 ms 原生测试跨多个周期保持同一权限，车端
  代续被拒绝，停止刷新后仍按期释放且审计不含 token；真实 Chromium 用 2 秒租约跨越
  8 秒仍保持 `session-000001`、WSS connected 和服务端单一权限，产生 17 次续租，随后
  安全退出归零，浏览器全程 0 error、0 warning；
- 页面使用内联 favicon，新的 Chromium 首屏复验为 0 console error、0 warning；
- 标准 Gamepad 与可配置方向盘/踏板映射、死区、反向、中心/静止位和量程
  校准已经接入统一控制状态；无设备、断开和失焦都会归零；
- 监控面板显示车辆/会话/控制权、编码/后端、控制 RTT、Direct/STUN/TURN、
  时间同步以及逐路 FPS/码率/丢包/端到端时延，并对 200 ms、20 FPS、控制权
  丢失和时间不可信给出明显告警；
- 非回环服务器地址强制使用 HTTPS/WSS；包内 CA bundle 由启动脚本传给 C++
  运行时，并由运行时显式应用到 HTTPS API 与 WSS 的 `libcurl` handle；
- 信令已使用真实 WebSocket push/ack：本机测试证明车端 offer 推送、Mac answer
  回送、参与者隔离、控制权过期后本地清权，以及 WSS 强制断开后复用仍有效的
  原会话重连；另用 Caddy 内部 CA 在单一 TLS 入口完成一次未关闭证书校验的
  HTTPS/WSS 纵向验收；
- 信令 push 带单调 delivery cursor，服务端保留到客户端确认；断线前未确认消息
  重连后补发并在客户端去重。控制端发送 ACK 不确定时复用原 `seq/message_id`
  自动重试，服务端返回稳定幂等确认且只入队一次；同序列不同内容仍拒绝；8 个
  并发浏览器 ICE 请求经单一发送序列化后全部到达，序号无乱序；
- 本机实际停止再启动 Caddy 容器后，短时中断保留原 `session-000002`，恢复后只
  收到一次中断期间入队的 offer，WSS 重连计数为 1；超过服务端心跳门限的中断
  则正确撤销旧控制权。初始状态 JSON 也已固定为 `[]`/`{}`，不再出现嵌套数组；
- 服务启停、WebSocket 生命周期和重连测试通过普通构建、ASan/UBSan 与 TSan；
  TSan 找到的监听句柄停止竞态已修复并通过复测；
- 原生重启门会真实停止并销毁 signaling listener，再在同一端口启动新的服务实例；
  旧 driver token、session 和 control token 均被拒绝，长驻控制端清除旧 WSS/控制权，
  识别新的 `service_instance_id` 后使用仅保存在原生进程内的凭据自动重新认证；传输
  中断期间车辆列表只返回强制 `online=false`、`controllable=false` 的安全旧快照，且
  不恢复旧 session/控制权。新建会话后仅新 control token 可用。该门已通过普通、
  ASan/UBSan 与 TSan；
- Mac 控制测试同时覆盖服务端登录防暴力破解：阈值前返回 401，达到阈值和锁定期
  返回带 `Retry-After` 的 429，锁定到期恢复；未知账号共享有界桶，12 路并发失败
  精确得到 3 次 401 和 9 次 429，且审计不含密码或攻击者提交的未知账号名；审计
  路径不可写时请求失败关闭，但锁定状态仍然建立，后续有效密码也不能绕过；
- 通用 HTTP/WSS 来源配额测试覆盖可信代理 IP、`X-Forwarded-For` 伪造隔离、NAT
  共享、来源表硬上限与溢出桶、窗口恢复、最大整数窗口和 12 路并发；最终实现通过
  普通、ASan/UBSan、TSan、Linux arm64 全量 CTest，以及本地 Caddy HTTPS/WSS
  200/200/429 与 `Retry-After`/`no-store` 验收；
- Mac 默认测试集还会把设备凭据、驾驶员 token、TURN REST credential 和控制
  token 交叉提交到错误用途：全部业务身份冒用返回 401；会话替换后旧控制 token
  在车端接收器返回 `control_token_invalid`，新 token 才能通过。该回归已通过普通、
  ASan/UBSan 与 TSan；长期凭据哈希、轮换和集中存储仍不在本机证明范围内；
- 服务端为 HTTP 2xx/4xx/429 和已识别的 WSS 101/拒绝握手生成
  `X-Request-ID`，同一请求写出的审计记录使用相同 `request_id`；伪造客户端值不会
  被信任或反射，16 路并发请求没有 ID 串线。该实现已通过普通、ASan/UBSan、TSan、
  Linux arm64 全量 CTest，以及本地 Caddy HTTPS/WSS 真实响应头验收；
- signaling 审计写入由专用 mutex 串行化，每个 UTC 小时归档一次并默认保留
  最近 7 天；当前小时仍受 1 到 20 个大小分片限制。启动先写 UTC
  `signaling_service_started`，目录缺失或不可写时直接启动失败。原生测试覆盖
  16 路并发写入、逐行 JSON 解析、小时切片、过期删除、六日内保留和大小上限；
- signaling `/health` 在当前登录锁定或来源表溢出时返回无身份信息的固定告警码并
  切换为 `degraded`，窗口到期后自动恢复 `ok`；原生测试覆盖正常、告警和恢复三态；
- 真实 Chromium 登录在线模拟车并建立 `session-000001` 后，从页面同源调用
  `/api/webrtc/metrics` 上报 `connected/direct`：首次返回
  `reported=true`/`webrtc_connection_succeeded`，相同样本再次返回 `reported=false`；
  signaling JSONL 恰好一条成功事件，带 request/session/vehicle/driver/service ID、
  Direct 路径和可信时间信息，不含本次驾驶员/设备凭据。安全退出后
  `active_sessions=0`、`online_drivers=0`，浏览器为 0 error、0 warning；
- 每次 signaling 运行生成独立 `service_instance_id`，同一运行的 `/health`、请求
  审计和后台启动/reaper 审计使用同一值，重启后更换；轮转重启测试验证未复用；
- 页面事件 JSONL 的大小轮转、递归凭据字段脱敏和非法输入拒绝通过；
- 自动成品门禁确认生成包的校验和、签名、架构、系统依赖、解压启动、回环监听、
  占用端口报错、页面脚本、本地日志和退出端口释放。

## 尚未验收

- macOS x64 运行验收（Apple Silicon 无 Rosetta 时可生成带明确 build-only
  标记的交叉编译包）；当前主机执行 `arch -x86_64 /usr/bin/true` 返回
  `Bad CPU type in executable`，不能把交叉编译当作运行通过；
- Apple Developer ID 签名与 notarization；当前钥匙串的 code-signing identity
  数量为 0，只能执行已明确标记的 ad-hoc 签名；
- 公网可信域名 `60-205-213-254.sslip.io` 的 TLS 健康检查和 Mac 登录已通过，云端
  signaling 与 TURN 已启动；实际部署代理后的有会话 WSS 断线重连、游标持久化、
  公网强制 TURN，以及真实 GStreamer 车端/相机的多路视频和 DataChannel 仍待车端
  恢复后验收。本机内部 CA 与浏览器车端模拟器不能替代这些现场门禁；
- 使用一只真实 Gamepad 完成 macOS 输入验收，以及非标准方向盘的平台适配器；
  当前 `hidutil list` 没有 Usage Page 1 的 Joystick、Gamepad 或 Multi-axis 控制器，
  Logitech USB Receiver 只暴露键盘/鼠标用途，不能冒充手柄验收；
- Windows 与 Ubuntu 控制端包。

这些项目仍保留在 `docs/20-three-end-taskbook-status.md`，不得用本次本机回环
验收替代现场或三平台验收。
