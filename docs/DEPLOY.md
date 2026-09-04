# Mine Teleop 部署教程

本文只描述如何安装和启动已经构建好的三端安装包。如何生成安装包见
[BUILD.md](BUILD.md)。

## 1. 部署顺序

按以下顺序执行：

1. 云端：安装信令、Caddy、HAProxy 和 Coturn；
2. 车端：解压安装包、放置 device token、检查配置并前台启动；
3. 控制端：解压 macOS 包，启动本地 `127.0.0.1` 控制页面；
4. 联合验收：先直连/STUN，再强制 TURN，最后接入真实相机、编码器和底盘。

部署不会在工控机上编译源码。

## 2. 首次部署凭据

在可信管理机上生成一次凭据，不要提交到 Git：

```bash
cd /path/to/mine-teleop
umask 077
mkdir -p .local/deployment/identity-secrets

openssl rand -base64 32 \
  > .local/deployment/identity-secrets/driver-console-001.password
openssl rand -hex 32 \
  > .local/deployment/identity-secrets/vehicle-001.token
openssl rand -hex 32 \
  > .local/deployment/turn-static-auth.secret

cp packaging/ubuntu-cloud/signaling-server.yaml.example \
  .local/deployment/signaling-server.yaml
```

凭据对应关系：

- `driver-console-001.password`：控制页面登录密码；
- `vehicle-001.token`：云端和车端共享的车辆设备凭据；
- `turn-static-auth.secret`：云端信令和 Coturn 共享的 TURN REST secret。

生产环境不要使用公开的 `dev-password` 或 `dev-device-secret`。

## 3. 部署云端

目标系统：Ubuntu 22.04 amd64、systemd、可用的公网 TLS/WSS 和 TURN 端口。

上传并解压云端包：

```bash
cloud_bundle="$(ls -t dist/mine-teleop-cloud-ubuntu22.04-x64-*.tar.gz | head -n 1)"
scp "$cloud_bundle" user@cloud:/tmp/mine-teleop-cloud.tar.gz
ssh user@cloud

mkdir -p ~/mine-teleop-cloud
tar -xzf /tmp/mine-teleop-cloud.tar.gz \
  -C ~/mine-teleop-cloud \
  --strip-components=1
cd ~/mine-teleop-cloud
```

将以下文件通过受保护通道上传到云端暂存目录：

```text
/secure/staging/signaling-server.yaml
/secure/staging/identity-secrets/driver-console-001.password
/secure/staging/identity-secrets/vehicle-001.token
/secure/staging/turn-static-auth.secret
```

首次安装前先检查包内 Caddy/HAProxy 配置中的公网地址和域名，然后执行：

```bash
sudo ./deploy-cloud.sh \
  --signaling-config /secure/staging/signaling-server.yaml \
  --identity-secrets-dir /secure/staging/identity-secrets \
  --turn-secret-file /secure/staging/turn-static-auth.secret \
  --turn-realm YOUR_TURN_REALM \
  --turn-host YOUR_TURN_HOST \
  --caddy-config deployments/caddy/Caddyfile.three-machine \
  --haproxy-config deployments/haproxy/haproxy.three-machine.cfg
```

安装但暂不启动：

```bash
sudo ./deploy-cloud.sh --no-start
```

后续仅升级应用并复用已有配置：

```bash
sudo ./deploy-cloud.sh
```

云端验收：

```bash
sudo systemctl is-active mine-teleop-cloud.target
curl -fsS http://127.0.0.1:8765/health
sudo journalctl -u mine-teleop-signaling-server -n 100 --no-pager
```

## 4. 部署车端工控机

目标系统：Ubuntu 22.04 amd64。车端运行不依赖 Docker、Python、源码仓库或
systemd。

### 4.1 最简单的手工部署

先从管理机上传安装包和与云端完全相同的车辆 token：

```bash
vehicle_bundle="$(ls -t dist/mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz | head -n 1)"
scp "$vehicle_bundle" vehicle@industrial-pc:/tmp/mine-teleop-vehicle.tar.gz
scp .local/deployment/identity-secrets/vehicle-001.token \
  vehicle@industrial-pc:/tmp/vehicle-001.token
ssh vehicle@industrial-pc

mkdir -p ~/mine-teleop
tar -xzf /tmp/mine-teleop-vehicle.tar.gz \
  -C ~/mine-teleop \
  --strip-components=1
cd ~/mine-teleop
```

在工控机上安装 token，并删除暂存副本：

```bash
install -m 600 /tmp/vehicle-001.token config/device-token
rm -f /tmp/vehicle-001.token
```

检查 `config/vehicle-agent.yaml` 中的车辆 ID、云端 URL、CA、相机和底盘配置。
然后执行：

```bash
./bin/mine-teleop-run version
./bin/mine-teleop-run config-check --config config/vehicle-agent.yaml
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --preflight
```

Basler USB3 Vision 首次授权：

```bash
sudo ./scripts/setup_basler_usb_access.sh "$USER"
```

重新登录后，车端只需要一个前台启动命令：

```bash
sudo install -d -m 0750 -o "$(id -un)" -g "$(id -gn)" /var/log/mine-teleop
./bin/mine-teleop-run
```

它会自动加载 `config/vehicle-agent.yaml`、`config/device-token`、打包内动态库
和 GStreamer 插件，并同时监管控制与媒体进程。使用 `Ctrl-C` 停止。终端
stdout/stderr 同时合并保存为 `/var/log/mine-teleop/vehicle-runtime.log`（单文件
64 MiB、保留 `.1` 到 `.5`）；VCU/CAN 高频证据继续使用独立的
`vcu-can.jsonl*` 轮转集合。

### 4.2 从管理机自动部署

```bash
scripts/deploy/deploy_vehicle_bundle.sh \
  --bundle dist/mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz \
  --host INDUSTRIAL_PC \
  --user VEHICLE_USER \
  --ssh-key ~/.ssh/id_ed25519 \
  --device-token-file .local/deployment/identity-secrets/vehicle-001.token
```

先查看操作但不连接：

```bash
scripts/deploy/deploy_vehicle_bundle.sh \
  --host INDUSTRIAL_PC \
  --user VEHICLE_USER \
  --dry-run
```

部署脚本只是 SSH 上传和验收辅助工具，不是车端运行依赖。

## 5. 部署 macOS 控制端

解压：

```bash
tar -xzf mine-teleop-control-macos-*.tar.gz
cd mine-teleop-control-macos-*
```

使用字段配置启动：

```bash
./run-control.command \
  --config config/driver-console.three-machine.yaml
```

程序仅监听 `127.0.0.1`，自动打开浏览器。控制页面输入
`driver-console-001.password` 的内容。

自定义本地端口：

```bash
./run-control.command \
  --config config/driver-console.three-machine.yaml \
  --port 28080
```

然后访问：

```text
http://127.0.0.1:28080
```

不要把司机密码写入 YAML、命令行历史或安装包。

## 6. 联合启动与验收

启动顺序：

1. `mine-teleop-cloud.target` 健康；
2. 车端 `./bin/mine-teleop-run` 已注册并保持在线；
3. 控制端登录并选择 `vehicle-001`；
4. 确认控制权、视频轨道、时间同步和 DataChannel，并等待页面显示车端已确认 V3 会话控制参数及匹配的 `applied_revision`；
5. 释放会话后确认车辆回到安全状态。

推荐检查：

```bash
# 云端
curl -fsS http://127.0.0.1:8765/health

# 车端
./bin/mine-teleop-run config-check --config config/vehicle-agent.yaml

# 控制端
curl -fsS http://127.0.0.1:8080/health
```

真实底盘上线前必须单独验收 CAN 反馈、制动、断链安全停车和本地急停。
控制页面默认的 `300 Nm/路`、`100 bar/路` 以及软件/配置测试结果都不是实车能力或
压力标定证据。普通驾驶前应在隔离台架确认 profile ACK 的全部 effective V3 字段、
`applied_revision == request.seq`、目标车速 PID、
页面只读的原始 `max_speed_kph`/`max_throttle`、速度反馈超时、硬超速余量、watchdog/
减速曲线，CAN 请求、实测反馈、缓刹/急刹压力和断链撤销；不得用页面显示“已确认”替代硬件验收。

## 7. 升级和回滚

本版本启动前先迁移仓库外配置：

- 从驾驶端 YAML 删除已废弃的 `control.keyboard` 整段。键盘固定为方向键/
  `WASD`、`Space` 缓刹、`B` 急刹和 `E` 急停；保留旧段会使控制端启动失败。
- 删除驾驶端旧的 `initial_max_throttle`、`initial_service_brake`、
  `initial_hard_brake`、`initial_max_brake` 和 `initial_max_brake_request`。加载器不会把
  比例静默解释成 km/h 或 bar；按配置文档显式填写 `initial_target_speed_kph`、
  `initial_max_motor_torque_nm` 以及最大普通/缓刹/急刹三个 `*_brake_pressure_bar`。
- 三项普通制动值表示每路 EHB 压力，必须满足 `0 <= service <= hard <= max`，控制端
  schema 上限为 `327.6 bar/路`；急停、物理急停、故障、断开停车和 bridge 本地 apply
  watchdog 的 `409.5 bar/路` 安全请求不属于会话 profile。上游控制心跳超时先按
  `deceleration_profile` 的 0.3/0.6 普通压力分段执行，最终 1.0 阶段才切到 409.5 bar。
  单电机转矩的控制端 schema 上限为 `640.0 Nm/路`。两者仍会被具体车辆 hard
  limits 下调，不能通过控制端设置提高车端上限。
- `session_control_profile` 已升级为 `profile_version=3`：除原五项驾驶参数外，新增
  `max_steering_angle_deg`、`speed_pid_kp/ki/kd`、
  `speed_pid_derivative_filter_tau_ms`、整数 `speed_pid_max_dt_ms` 和
  `motor_torque_rise_rate_nm_per_s`。控制端 YAML 不配置
  PID/升扭默认值；车端必须通过 `control_limits` 上报五个 `default_speed_pid_*` 字段、
  嵌套 `speed_pid_limits` min/max、`default_motor_torque_rise_rate_nm_per_s` 与
  `motor_torque_rise_rate_limits_nm_per_s` min/max，以及顶层速度反馈超时、硬超速余量和 exact
  `read_only_control_safety`（20 Hz 上游命令频率、命令间隔、watchdog、减速曲线、反馈超时、
  超速余量、CAN/急停/时间同步门禁和 commissioning mode）。顶层与对象中的重复值必须一致。
  缺少完整默认值/边界/固定安全对象、`kp <= 0` 或 ACK 缺少匹配的正整数
  `applied_revision` 时，页面保持 fail closed。首次 profile、任一 PID 修改及转向上限任意
  变化只能在 `parking_ready` 且 VCU 为 `standby`/`disarmed`，或明确的隔离 mock bench
  条件下提交。
- 将所有写旧 `/api/control-limits` 的集成迁移到 `/api/control-profile`。旧 GET 仅保留
  归一化比例只读兼容，旧 POST 固定返回 `410 Gone`，不会再直接修改当前会话制动参数。
- 将旧 `POST /api/control/keyboard`、`POST /api/control/gamepad` 调用迁移到标准
  `/api/control`；两个 specialized endpoint 现在固定返回 `410 Gone`，避免在 profile
  尚未被车端 ACK 或已被拒绝时按错误的物理压力比例发送命令。
- 检查车端 `field_safety.max_speed_kph`：所有 adapter 的范围均为 `[0, 72] km/h`，
  `0` 明确禁用牵引。旧配置如果大于 `72` 会启动失败，必须根据本地 PID 车速上限和
  隔离台架结果显式修改，不得为通过校验而盲目压到 `72`。
- 重新计算非 mock 车端的 `full_scale_motor_torque_nm`。旧语义的有效每路上限约为
  `full_scale_motor_torque_nm × max_throttle`，新语义中 `full_scale_motor_torque_nm` 本身就是唯一上限。
  例如旧 `41.25 × 0.10 ≈ 4.125 Nm/路`，不得原样保留 `41.25 Nm`。要从旧有效上限起步并
  重做隔离台架标定；缺少新版非 mock 必填安全字段时应保持启动失败，不得补默认值绕过。

升级与回滚时还要遵守：

- 云端部署脚本会备份被替换的应用目录和配置；升级后先检查 loopback health；
- 车端升级前保留上一版 `.tar.gz` 和 `.sha256`，失败时停止当前进程并重新解压
  上一版；
- 控制端保留上一版目录，退出新版本后可直接运行旧版
  `run-control.command`；
- 凭据轮换时必须同步更新云端与对应车辆或控制端，并重启相关进程；
- 不要在仍有驾驶会话时执行升级或凭据轮换。

更多故障处理见
[12-operations-and-troubleshooting.md](12-operations-and-troubleshooting.md)。
