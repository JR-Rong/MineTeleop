# 控制端 Docker 程序与 Smoke

> 迁移说明：当前 smoke 全部使用原生 C++ 容器，入口为 `scripts/run_control_plane_docker_smoke.sh`。

`deployments/container/Dockerfile.control` 现在默认启动控制端完整程序入口：

```bash
python3 -m mine_teleop.control_console_container
```

该入口不要求调用方重写容器 command；现场部署通过环境变量注入配置：

| 环境变量 | 默认值 | 用途 |
| --- | --- | --- |
| `MINE_TELEOP_DRIVER_CONSOLE_CONFIG` | `configs/driver-console.dev.yaml` | 驾驶端配置文件 |
| `MINE_TELEOP_DRIVER_CONSOLE_HOST` | `0.0.0.0` | HTTP UI/API 监听地址 |
| `MINE_TELEOP_DRIVER_CONSOLE_PORT` | `8080` | HTTP UI/API 监听端口 |
| `MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL` | 配置文件中的 `cloud.signaling_url` 转换为 HTTP | 信令服务 HTTP 基地址 |
| `MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID` | `vehicle-001` | 默认连接车辆 |
| `MINE_TELEOP_DRIVER_CONSOLE_PASSWORD` | `dev-password` | 驾驶员登录口令 |
| `MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG` | `/tmp/mine-teleop-driver-console/operation-log.jsonl` | 本地操作日志 |
| `MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_MAX_BYTES` | `10485760` | 操作日志轮转大小 |
| `MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_BACKUP_COUNT` | `5` | 操作日志保留份数 |
| `MINE_TELEOP_DRIVER_CONSOLE_FRAME_DIR` | `/tmp/mine-teleop-driver-console/frames` | 解码帧缓存目录 |
| `MINE_TELEOP_DRIVER_CONSOLE_CONTROL_OUTPUT` | 空 | 调试时把控制命令写入 JSONL，而不走信令 |

控制端在 Docker 内提供 HTTP UI/API：

- `GET /`：驾驶端操作界面。
- `GET /api/status`：会话、视频面板、工具栏和状态栏快照。
- `POST /api/connect`：驾驶员登录并向信令服务请求车辆会话。
- `POST /api/poll-signaling`：接收车端 `webrtc_offer`，更新前端视频面板状态，并把
  offer/ICE candidate payload 返回给浏览器 WebRTC 客户端。页面连接后每秒轮询一次。
- `POST /api/webrtc/answer`：浏览器 `RTCPeerConnection` 生成 answer 后，经控制端
  转发到车端收件队列。
- `POST /api/webrtc/ice-candidate`：浏览器 ICE candidate 经控制端转发到车端。
- `POST /api/control`：由软件控件状态生成 20 Hz `control_command`，并转发到车端收件队列。
- `POST /api/control/keyboard`：由页面键盘状态按 20 Hz 节流生成 `control_command`。
- `POST /api/control/gamepad`：由浏览器 Gamepad API 读取模拟驾驶器/手柄轴值后生成
  20 Hz `control_command`，不要求 Docker 容器直接访问宿主 HID 设备。
- `POST /api/media/frame`：接收 H.264 Annex-B payload，解码为最新 PNG 帧，并返回当前 camera 的
  `frame_sequence`。

`configs/driver-console.dev.yaml` 中的 `control.gamepad` 暴露模拟驾驶器映射：

```yaml
gamepad:
  enabled: true
  steering_axis: 0
  throttle_axis: 2
  brake_axis: 5
  axis_deadzone: 0.05
  throttle_inverted: true
  brake_inverted: true
  estop_button: 0
```

本地交互式启动完整控制端 Docker 程序：

```bash
scripts/run_control_plane_docker.sh
```

该脚本会使用同一镜像启动本地 `signaling-server` 和 `driver-console --serve`。
`signaling-server` 只监听容器 loopback，`driver-console` 通过共享网络命名空间访问它；
宿主机只暴露驾驶端 UI/API 端口，脚本会输出：

```text
DRIVER_CONSOLE_URL=http://127.0.0.1:8080
```

浏览器打开 `DRIVER_CONSOLE_URL` 后可进入驾驶端页面。脚本以前台方式运行，
按 `Ctrl-C` 会清理本次启动的容器；它不会写入 systemd、不会配置 Docker restart
policy，也不要求 Docker Compose。

本地浏览器级验收可以使用：

```bash
scripts/run_control_plane_browser_smoke.sh
```

该脚本会启动上面的 Docker 控制端栈，再用宿主机 Chrome/Chromium 打开驾驶端页面，
并创建一个独立 Chrome target 作为车端 WebRTC 模拟端。模拟端的
`webrtc_offer`、remote ICE candidate 以及控制端生成的 answer/local ICE candidate
均通过 Docker 内 loopback `signaling-server` 的 HTTP signaling queue 中转；
脚本用 `docker exec` 访问 signaling 容器，不需要把 signaling 端口发布到宿主机。
随后脚本执行 `connectConsole()`、键盘控制和软件控制，最后保存
`browser-smoke-summary.json` 与 `operator-page.png`。通过时关键字段为：

```json
{
  "passed": true,
  "session_state": "SESSION_ACTIVE",
  "keyboard_control_command_sent": true,
  "software_control_command_sent": true,
  "control_command_sent": true,
  "vehicle_peer_offer_forwarded_via_signaling": true,
  "webrtc_answer_received_via_signaling": true,
  "local_ice_candidate_received_via_signaling": true,
  "remote_ice_candidate_forwarded_via_signaling": true,
  "loopback_webrtc_media_received": true,
  "datachannel_control_command_received": true,
  "operator_session_state_text": "SESSION_ACTIVE",
  "operator_control_authority_text": "active",
  "operator_camera_summary_text": "1/4 connected",
  "operator_datachannel_state_text": "sent seq 4",
  "webrtc_available": true,
  "gamepad_mapping_present": true
}
```

该浏览器 smoke 证明控制端页面 JavaScript 能在真实浏览器中执行连接和控制路径，
并用独立模拟车端 peer 经真实 signaling queue 验证控制端页面可接收远端
WebRTC video track，也可通过 unordered/unreliable `control` DataChannel
把控制命令交给模拟车端。
同时 smoke 会读取页面右侧操作员状态面板，验证 session、控制权、摄像头连接数量、
最近控制命令和 DataChannel 状态确实渲染到界面上，而不是只存在于 `/api/status`
的 raw JSON 中。
它仍不替代真实车端进程、真实摄像头 RTP 输入和真实 CAN 输出的现场验证。

如果要连接已有信令服务，也可以只启动控制端容器：

```bash
MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL=http://host.docker.internal:8765 \
scripts/run_driver_console_docker.sh
```

默认会把控制端 UI 暴露到 `http://127.0.0.1:8080`。脚本只前台运行一个容器，
不安装 systemd 服务，也不要求 Docker Compose。

本 smoke 使用同一个镜像在本地 Docker 内启动 loopback `signaling-server`、
真实 `driver-console --serve` 控制端容器，再用一次性 `control-plane-smoke`
容器验证两条链路：

- WebRTC 信令链路：车端身份向驾驶端发送 `webrtc_offer`，控制端 HTTP 程序轮询信令队列并把
  offer payload 交给浏览器；浏览器页面包含 `RTCPeerConnection`、`ontrack` 视频挂载和
  unordered/unreliable `control` DataChannel wiring；smoke 使用 `/api/webrtc/answer`
  验证 answer 会转发到车端收件队列，并验证车端 remote ICE candidate 可被控制端轮询、
  控制端 local ICE candidate 可转发到车端。
- 收图解码链路：控制端把前摄状态更新为 `connected`；smoke 再向控制端连续提交两帧
  H.264 Annex-B 编码样本，控制端用
  `ffmpeg` 解码后通过 `/api/frame/front.png` 提供 PNG，artifact 保存为
  `front-received.png`，并在 `/api/status` 暴露 `decoded_frame_count_by_camera.front=2`。
- 控制链路：控制端 HTTP API 接收软件控件状态和模拟驾驶器轴值，生成 20 Hz
  `control_command`，通过当前 Docker/local signaling relay 转发给车端收件队列；
  消息内带当前会话 `authority_token`，信令服务会校验 sender、recipient、
  vehicle_id、session_id 和 token。

该流程只使用开发配置，不连接真实 CAN，不发送真实底盘控制帧。smoke 会验证
WebRTC offer/answer 信令和浏览器页面 wiring，但不会在本机证明真实 RTP 包、
真实浏览器 DataChannel 或真实车端 WebRTC peer 已端到端工作。控制端镜像不安装
Intel VAAPI driver，避免在 Apple Silicon/arm64 本机构建时依赖车端硬编包。
镜像构建时会复制验证所需源码和开发配置，运行时不依赖宿主机仓库目录 bind mount，
用于避开本机 Docker 对 `/Volumes/SystemDisk` 共享不完整的问题。

运行方式如下。不需要 Docker Compose 插件，只使用本机 `docker` CLI：

```bash
scripts/run_control_plane_docker_smoke.sh
```

通过时 `control-plane-smoke` 会输出一行 JSON，其中关键字段为：

```json
{
  "passed": true,
  "control_console_received_image": true,
  "vehicle_received_steering": true,
  "vehicle_received_acceleration": true,
  "vehicle_received_deceleration": true,
  "signaling": {
    "media_offer_received": true,
    "webrtc_answer_forwarded": true,
    "remote_ice_candidate_received": true,
    "local_ice_candidate_forwarded": true
  },
  "media": {
    "frame_received": true,
    "frame_sequences": [1, 2],
    "decoded_frame_count_by_camera": {"front": 2}
  },
  "control": {
    "commands_generated": 5,
    "software_control_commands": 3,
    "gamepad_control_commands": 2,
    "commands_forwarded": 5,
    "vehicle_received_commands": 5,
    "vehicle_received_steering": true,
    "vehicle_received_acceleration": true,
    "vehicle_received_deceleration": true,
    "authority_token_present": true
  }
}
```
