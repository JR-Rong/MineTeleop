# 运维与排障

本页只保留当前原生 C++ 三端路径。现场拓扑和已验证启动顺序见
`docs/22-three-machine-live-delivery.md`。

## 运行边界

- 云端：signaling、Coturn、Caddy 和 HAProxy 使用独立 systemd 服务，由
  `mine-teleop-cloud.target` 统一启停。
- Ubuntu 车端：使用自包含压缩包和前台 `mine-teleop-run`，不安装车端
  systemd，不依赖 Docker、Python 或 FFmpeg。
- 控制端：本地原生进程只监听 `127.0.0.1`，页面由系统浏览器打开。
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

signaling 必须只绑定回环地址，由 Caddy 终止 TLS。公网不得直接开放 8765。

## Ubuntu 车端

```bash
tar -xzf mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz
cd mine-teleop-vehicle-ubuntu22.04-x64-*
printf '%s\n' '<device-token-from-secret-store>' > config/device-token
chmod 600 config/device-token

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
生成，并在运行前通过 `config-check`。

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
scripts/render_turnserver_config.sh \
  --realm turn.example.com \
  --secret-file /etc/mine-teleop/secrets/turn-static-auth.secret \
  --output /etc/mine-teleop/turnserver.conf
```

检查中继：

```bash
scripts/check_coturn_relay.sh
scripts/coturn_usage_report.sh \
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
