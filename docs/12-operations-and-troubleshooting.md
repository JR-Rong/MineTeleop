# 运维与排障

本页只保留当前原生 C++ 三端路径。现场拓扑和已验证启动顺序见
`docs/22-three-machine-live-delivery.md`。

## 运行边界

- 云端：signaling、Coturn、Caddy 和 HAProxy 使用独立 systemd 服务，由
  `mine-teleop-cloud.target` 统一启停。
- Ubuntu 车端：使用自包含压缩包和前台 `mine-teleop-run`，不安装车端
  systemd，不依赖 Docker、Python 或 FFmpeg。
- 控制端：本地原生进程只监听 `127.0.0.1`，页面由系统浏览器打开；浏览器只提交
  最新输入意图，原生进程独立以 20 Hz 经专用 WSS 发包。
- 正常驾驶链路直接连接云端 HTTPS/WSS/STUN/TURN，不依赖 FRP、SSH 反向隧道
  或 SOCKS。

## 云端

安装方法见 `deployments/systemd/README.md`。常用命令：

```bash
sudo systemctl start mine-teleop-cloud.target
sudo systemctl restart mine-teleop-cloud.target
sudo systemctl stop mine-teleop-cloud.target
sudo systemctl --no-pager --full status \
  mine-teleop-cloud.target \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service
```

服务异常时分别查看日志：

```bash
sudo journalctl -u mine-teleop-signaling-server.service -n 200 --no-pager
sudo journalctl -u mine-teleop-turn-server.service -n 200 --no-pager
sudo journalctl -u caddy.service -n 200 --no-pager
sudo journalctl -u haproxy.service -n 200 --no-pager
```

控制逐段时序写入 `/var/log/mine-teleop/signaling-audit.jsonl*`，事件名为
`cloud_native_control_trace_batch`。控制端的浏览器意图与原生 sender 时序共同写入包内
`.local/logs/control-browser-events.jsonl*`；车端接收/apply 时序写入
`/var/log/mine-teleop/vehicle-runtime.log*`。复现时三端文件必须覆盖同一
`trace_session_id`，再按 `seq`、`intent_seq` 和 `delivery_cursor` 对齐。

signaling 必须只绑定回环地址，由 Caddy 终止 TLS。公网不得直接开放 8765。

## Ubuntu 车端

```bash
tar -xzf mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz
cd mine-teleop-vehicle-ubuntu22.04-x64-*
printf '%s\n' '<device-token-from-secret-store>' > config/device-token
chmod 600 config/device-token
sudo install -d -m 0750 -o "$(id -un)" -g "$(id -gn)" /var/log/mine-teleop

./bin/mine-teleop-run config-check --config config/vehicle-agent.yaml
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --preflight
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --adapter-status
./bin/mine-teleop-run media-probe
./bin/mine-teleop-run
```

最后一条命令在前台监督控制和媒体进程；任一子进程失败时结束同伴进程。使用
`Ctrl-C` 停止。现场配置应从 `configs/vehicle-agent.three-machine.field.yaml`
生成，并在运行前通过 `config-check`。启动器继续把 stdout/stderr 显示在终端，
同时合并写入 `/var/log/mine-teleop/vehicle-runtime.log`；该混合日志不是严格
JSONL，默认每份 64 MiB、保留 `.1` 到 `.5`。可识别的 vendor ChassisControl 输出
（包括每周期裸行 `UpdateVehicleState` 及其 `[timestamp] [level] [pid] [tag]` 日志）
只留在终端，不写入该落盘日志；Mine Teleop 结构化诊断仍照常记录。VCU 原始协议日志仍单独写入
`vcu-can.jsonl*`。

实时查看车端运行日志：

```bash
sudo tail -F /var/log/mine-teleop/vehicle-runtime.log
```

## macOS 控制端

```bash
tar -xzf mine-teleop-control-macos-arm64-*.tar.gz
cd mine-teleop-control-macos-arm64-*
./run-control.command --config config/driver-console.three-machine.yaml
```

密码在回环页面输入，或通过 `MINE_TELEOP_DRIVER_PASSWORD` 提供；不要写入 YAML
或 shell 历史。端口被占用时先查找旧进程，不要改成公网监听：

```bash
lsof -nP -iTCP:8080 -sTCP:LISTEN
```

## 配置与凭据

- device token 使用权限为 `0600` 的独立文件；
- signaling 多身份配置只保存 ID、allowlist 和 secret 来源；
- TURN shared secret 使用独立文件；
- 日志、命令行和文档中不得记录 token、密码或完整预签名 URL；
- 修改 YAML 后先执行 `config-check`，再执行 `preflight`。

## TLS/WSS

检查公网入口和后端：

```bash
curl -fsS https://teleop.example.com/health
curl -fsS http://127.0.0.1:8765/health
ss -lntup
```

预期状态：

- 公网只有批准的 TLS/TURN 端口；
- signaling 监听 `127.0.0.1:8765`；
- 控制端和车端校验证书，不使用 `-k`；
- Caddy 只转发 API/WSS 路径，其它路径返回 404。

若 HTTPS 正常但 WSS 失败，依次检查 Caddy upgrade 转发、证书主机名、应用内
解析、signaling audit 的 `request_id`，以及控制端是否连接了预期端口。

## TURN

渲染生产配置：

```bash
scripts/deploy/render_turnserver_config.sh \
  --realm turn.example.com \
  --secret-file /etc/mine-teleop/secrets/turn-static-auth.secret \
  --output /etc/mine-teleop/turnserver.conf
```

检查中继：

```bash
scripts/test/check_coturn_relay.sh
scripts/test/coturn_usage_report.sh \
  --log /var/log/mine-teleop/coturn.log \
  --require-relay-bytes
```

模板文件不能直接作为生产配置。公网验收必须确认实际 relay candidate、relay
字节和云端 UDP 端口范围，而不只是 STUN 成功。

## 相机与编码

```bash
ls -l /dev/video* /dev/dri 2>/dev/null
v4l2-ctl --list-devices
vainfo --display drm --device /dev/dri/renderD128
./bin/mine-teleop-run media-probe
```

Basler USB 权限使用：

```bash
sudo ./scripts/setup_basler_usb_access.sh <vehicle-user>
```

重新登录后再启动运行时。若探针通过但浏览器无画面，检查每路 camera selector、
GStreamer plugin path、signaling offer/answer/ICE 审计和浏览器 WebRTC stats。
摄像头节点、编码器、offer/answer/ICE/DataChannel 的完整 `issue_code` 与过滤命令
见 `docs/24-vehicle-runtime-diagnostics.md`。

## ChassisControl 与 CAN

bridge 构建命令见 `deployments/chassis-control-bridge/README.md`。部署前必须确认：

```bash
ip -details link show can0
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --preflight
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --adapter-status
```

动态 adapter 缺失、CAN interface 不存在或 bridge 打开失败时必须停止，不得
切回 mock。真实 CAN feedback、控制单位、安全停车和急停仍需在车轮离地或动力
隔离的安全台架上验收。
VCU/CAN 启动、日志落盘、收发、反馈超时、握手门禁和 disarm 的完整事件表见
`docs/24-vehicle-runtime-diagnostics.md`。

## 常见故障

### `config-check` 失败

先检查 YAML 类型、必填字段、相机 ID、cloud URL、证书路径、device token 文件
和 bridge 路径。不要绕过配置门禁直接启动。

### `preflight` 报相机缺失

核对配置中的 device selector 与实际 `/dev/video*`、Aravis 或 MVS 设备。USB
相机重新插拔后再次确认权限。

### `adapter-status` 失败

使用 `ldd` 检查 bridge 和 `libchassis_control.so` 的依赖，再检查 CAN interface
是否存在。不要用 `LD_LIBRARY_PATH` 指向未随包交付的开发 checkout。

### 服务重启后旧会话失效

这是预期行为。旧 driver、session 和 control token 不得复用；控制端应重新鉴权，
车端在重新注册前保持本地安全停车。

### `command_age_exceeded` / `command_gap_exceeded`

三机驾驶端配置默认启用批量控制追踪。复现后同时收集控制端包根目录
`.local/logs/control-browser-events.jsonl*`、控制端 `/api/status` 快照和车端 runtime
日志。先用 `trace_session_id`、`trace_vehicle_id` 确认浏览器意图属于哪个原始会话，
再用原生命令的 `session_id + seq` 对齐控制端 `native_control` 状态与车端
`vehicle_control_trace_batch.commands`；车端批次的 `dropped_total` 增长只表示诊断记录
自身有缺口。signaling 的控制 mailbox 容量为 1，中间序号被新命令覆盖是正常的，不能
要求每个控制端 `seq` 都逐条出现在车端：

- `control_trace_batch.summary.timer_lag_max_ms` 升高，但
  `/api/status.native_control.last_gap_ms` 仍接近 50 ms：只是浏览器输入租约刷新变慢；
  租约到期后应看到 `intent_fresh=false`、`requires_fresh_input=true` 和执行量归零，
  不应据此判断原生发包中断；
- `control_intent_update_timeout` 增长：检查浏览器到回环 `/api/control-intent` 的请求、
  本机进程和 CPU 调度。恢复页面后必须先提交新鲜中立输入，旧油门不会自动恢复；
- 控制端 `websocket_connected=false`、`send_failures_total` 增长、`last_ack_seq` 停滞，
  或 `last_gap_ms/max_gap_ms` 增大：检查控制端到 signaling 的专用 WSS、代理 Upgrade、
  ACK 停滞和重连退避；
- 控制端发送/ACK 继续推进但车端 `messages_received_total` 不增长：检查 signaling
  latest-only mailbox、车端 `vehicle_native_control_websocket_failed`、control-only WSS
  与 `post_error_discards_total`；
- 页面切走后 `stale_safe_heartbeats_accepted_total` 增长可以是预期安全心跳；
  `stale_nonzero_intent_discards_total`、`stale_safe_native_gap_discards_total` 或
  `stale_safe_untrusted_gear_discards_total` 增长则按对应门禁继续排查。零值 stale 包只能
  维持尚未中断的 `CONTROL_ACTIVE`，不能恢复已经发生的 `DEGRADED` 或真实 WSS 断流；
- profile/VCU/status DataChannel 关闭会撤销驾驶权限，但它不再是普通控制命令的传输
  路径。浏览器 WebRTC RTT 和视频 RTP 丢包率也不是控制 WSS 的逐命令送达证据。

### 磁盘增长

检查录像目录、sidecar、上传 archive 和配置的保留策略。删除现场录像前必须先
确认上传状态和回收范围。

## 回退

- 云端：停止 `mine-teleop-cloud.target`，恢复上一版二进制、配置和 unit，再整体
  启动并检查四个服务。
- 车端：停止前台运行时，切换到上一份校验和已确认的压缩包和配置；不要混用
  不同版本的二进制与动态库。
- 控制端：退出当前进程，解压上一份已验收包后重新登录。

回退完成后必须重新执行 health、登录、车辆上线、控制权、安全释放和至少一路
视频检查。
