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
./bin/mine-teleop-run
```

它会自动加载 `config/vehicle-agent.yaml`、`config/device-token`、打包内动态库
和 GStreamer 插件，并同时监管控制与媒体进程。使用 `Ctrl-C` 停止。

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
4. 确认控制权、视频轨道、时间同步和 DataChannel；
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

## 7. 升级和回滚

- 云端部署脚本会备份被替换的应用目录和配置；升级后先检查 loopback health；
- 车端升级前保留上一版 `.tar.gz` 和 `.sha256`，失败时停止当前进程并重新解压
  上一版；
- 控制端保留上一版目录，退出新版本后可直接运行旧版
  `run-control.command`；
- 凭据轮换时必须同步更新云端与对应车辆或控制端，并重启相关进程；
- 不要在仍有驾驶会话时执行升级或凭据轮换。

更多故障处理见
[12-operations-and-troubleshooting.md](12-operations-and-troubleshooting.md)。
