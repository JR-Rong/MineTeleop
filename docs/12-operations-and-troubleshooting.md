# 运维与排障

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

到工控机现场部署时，优先使用
[当前实现与工控机无 Docker 部署](14-current-status-and-ipc-deployment.md)
作为操作入口；本页保留更细的命令解释和排障背景。

## 运行方式

车端建议最终以 systemd 或容器方式运行。

systemd 目标：

- 开机自启动。
- 异常退出自动重启。
- 日志进入 journald 和文件。
- 环境变量固定。
- 车端 control/media/uploader 和 signaling 模板显式读取同一份
  `/etc/mine-teleop/vehicle-agent.yaml`；signaling 审计日志固定写入
  `/var/log/mine-teleop/signaling-audit.jsonl`，避免 systemd 下误用开发默认配置
  或相对审计路径。
- signaling systemd 模板显式使用 `--host 127.0.0.1`，公网入口应由 nginx、
  Caddy 或云负载均衡终止 TLS 后转发，不能依赖隐式默认绑定策略。
- systemd 模板使用 `LogsDirectory=mine-teleop` 创建 `/var/log/mine-teleop`；
  uploader 额外使用 `StateDirectory=mine-teleop/uploader` 创建
  `/var/lib/mine-teleop/uploader`，避免干净目标机上文件日志或上传队列目录缺失。
- uploader systemd 服务使用 `Nice=10`、`IOSchedulingClass=idle`、
  `CPUWeight=50` 和 `IOWeight=50`，使低优先级上传不与 control/media 争抢
  CPU/IO 调度优先级。

容器目标：

- 固定媒体栈版本。
- 挂载 `/etc/mine-teleop`，让 control/media/uploader 容器读取同一份
  `/etc/mine-teleop/vehicle-agent.yaml`。
- uploader 容器的 `cpu_shares` 低于 control/media 容器；`cpus`/`mem_limit`
  仍保留硬上限，确保上传压力不会压过实时控制和媒体。
- 挂载 `/dev/dri`。
- 挂载相机设备。
- 挂载录像目录。
- 限制资源。

### 云端统一启停入口

云端不要把 signaling、coturn、Caddy 和 HAProxy 合并到一个进程。仓库提供
`mine-teleop-cloud.target` 作为一个 systemd 操作入口，同时保留四个服务各自的
故障隔离、自动重启和日志。安装方法见
`deployments/systemd/README.md`。安装后常用命令为：

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

`target` 只合并生命周期操作，不吞并进程；其中一个守护进程失败时仍能从对应
`journalctl -u <unit>` 独立定位。

## 关键检查命令

### 目标车端配置生成

```bash
python3 scripts/render_chassis_vehicle_config.py \
  --base-config configs/vehicle-agent.dev.yaml \
  --output /etc/mine-teleop/vehicle-agent.yaml \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --can-interface can0 \
  --max-control-timeout-ms 900 \
  --calibration-evidence bench-brake-test-2026-06-24
```

该命令生成带 ChassisControl C shim、实际链接的 ChassisControl 动态库、
MinePilot CAN DB/receiver/sender 路径和 `control.timeout_calibration` 的车端配置。
生成后仍要按现场环境检查 cloud/TURN、
设备证书、相机设备、录像目录和上传目标；随后用本章的 preflight、
bridge 检查和 `--adapter-status` 证明配置可运行。

### 目标主机验证计划

```bash
python3 scripts/target_host_validation_plan.py \
  --vehicle-config /etc/mine-teleop/vehicle-agent.yaml \
  --hardware-device /dev/dri/renderD128 \
  --hardware-device /dev/dri/card1 \
  --can-interface can0 \
  --network-interface wwan0 \
  --chassis-control-branch UI_Test \
  --minepilot-branch merge_ui_test \
  --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --uploader-work-dir /var/lib/mine-teleop/uploader \
  --acceptance-samples /var/log/mine-teleop/acceptance-samples.jsonl \
  --acceptance-scenario target-host-acceptance \
  --artifact-dir /var/log/mine-teleop/target-validation \
  --format shell
```

该命令不会执行检查，只生成目标主机验证脚本或 JSONL 计划。计划覆盖 GPU/DRI、
VAAPI、媒体硬编探测、车端 preflight、CAN interface 状态、MinePilot CAN
source/socket/send 探针、ChassisControl/MinePilot bridge 检查、弱网矩阵
dry-run、上传器 service-mode 单次调度 smoke、硬编结果归档模板，以及统一验收指标报告模板。部署时应逐项执行并
保存输出，作为目标主机验收附件。
使用 `--format shell --artifact-dir <dir>` 时，生成的脚本会继续逐项执行所有
检查，把每项 stdout、stderr 和返回码写入 `<dir>`，并在
`target_host_validation_results.jsonl` 中记录 `target_host_validation_result`。
脚本末尾会追加 `target_host_validation_summary`，记录 required/optional 数量、
失败数量、`command_names`、`command_requirements`、`acceptance_scenario`、vehicle config、CAN interface、
ChassisControl/MinePilot checkout 路径和分支、`bridge_build_dir`、
`uploader_work_dir`、`minepilot_can_probe_build_dir`、正整数 `can_probe_timeout_seconds`，以及从
`chassis.bridge.check` stdout 提取的
`chassis_control_commit`、`minepilot_commit`、`chassis_control_dirty`、
`minepilot_dirty`、`bridge_library_path`、`chassis_control_library_path`
和对应 `changed_paths`
数量，以及整体 `passed` 状态。随后脚本会自动执行归档汇总报告，把
`target_host_validation_archive.jsonl` 写入同一个 `<dir>`，并以报告器退出码作为
最终退出码；required 项失败、附件缺失或反馈证据缺失时，脚本仍会先执行完后续
检查，再以退出码 2 标记验收未通过。
需要复核或从拷贝出的附件重跑汇总时，可手动执行同一个报告命令：

```bash
python3 scripts/target_host_validation_report.py \
  --results /var/log/mine-teleop/target-validation/target_host_validation_results.jsonl \
  --verify-artifacts
```
`--verify-artifacts` 会让报告同时检查每条记录引用的 stdout/stderr 日志文件
是否存在，并核对 `target_host_validation_summary` 中的 command/required/optional
数量、失败数量、`command_names`、`command_requirements`、summary `bridge_build_dir` 和 CAN probe
绑定字段是否与实际 result 记录一致；对于 `chassis.bridge.check`，还会核对
stdout summary `check_count`，以及 summary 的 commit/dirty revision summary
是否与 stdout 中的 checkout commit、dirty 和 changed_paths 一致；revision summary 缺失或 revision summary 无效时也会汇总
`passed=false` 并以退出码 2 返回。缺失 required 附件或 summary 不一致时同样
返回失败。
`vehicle.adapter.status` 会要求 `status.library_path` 与 summary
`bridge_library_path` 一致，避免 bridge 检查构建一个 C shim 而
vehicle-agent runtime 打开另一个 shim；summary `chassis_control_library_path`
记录该 shim 链接的底层 ChassisControl 动态库。
`vehicle.adapter.feedback_poll` 还会解析 stdout JSONL，要求同一命令输出中存在
real adapter `vehicle_adapter_status` ready 证据，以及
`vehicle_adapter_feedback_poll.received=true` 证据；缺少 stdout、缺少 adapter
status、CAN interface 不一致或没有收到反馈帧时，即使命令返回 0，归档汇总也必须
失败。
`network.weak.matrix` 归档会要求 dry-run 警告、54 个 documented weak-network
profile 和每个 profile 的 apply/clear 命令精确匹配；缺少 profile、混入额外 profile
或命令数量不匹配时，即使命令返回 0，归档汇总也必须失败。

### GPU 和编码能力

```bash
lspci -nnk | grep -EA4 'VGA|3D|Display|NVIDIA|Intel|AMD'
ls -l /dev/dri
nvidia-smi || true
vainfo --display drm --device /dev/dri/renderD128
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -encoders | grep -Ei 'vaapi|qsv|nvenc|x264'
gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc
python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-probes
```

目标主机验证计划中的 `media.hardware.probes` 会归档上述 GStreamer probe 和
3 个硬件压测场景；归档报告要求场景集合精确匹配，缺少场景、混入额外场景或
缺少 metrics 字段时都会失败。

### Docker VAAPI

```bash
sudo docker run --rm \
  --device /dev/dri/renderD128 \
  --device /dev/dri/card1 \
  -v /tmp:/out \
  ubuntu:22.04 \
  bash -lc 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ffmpeg vainfo intel-media-va-driver && vainfo --display drm --device /dev/dri/renderD128'
```

### 网络

```bash
ping <cloud-ip>
traceroute <cloud-ip>
iperf3 -c <server>
```

### S3 Upload Presign

部署使用 `upload.backend=s3` 时，先离线检查签名配置并归档脱敏 JSONL：

```bash
python3 scripts/upload_presign_report.py \
  --vehicle-config /etc/mine-teleop/vehicle-agent.yaml \
  --vehicle-id vehicle-001 \
  --session-id session-preflight \
  --camera-id front \
  --segment-id presign-smoke \
  --ttl-seconds 600
```

该命令会为 video 和 metadata 各生成一条样例预签名凭证，并只输出 endpoint、
bucket、region、对象路径、签名算法、过期时间和 signature/session-token 是否
存在；不会输出完整 URL、access key、secret 或 session token。真实 PUT、对象存储
权限策略和厂商兼容性仍需在部署环境中执行。

### TURN Usage Report

启动 systemd 或 Compose 前，必须先把 realm 与共享 secret 写入真实配置；不能把
`.template` 原样挂载给 Coturn：

```bash
scripts/render_turnserver_config.sh \
  --realm turn.example.com \
  --secret-file /etc/mine-teleop/secrets/turn-static-auth-secret \
  --output /etc/mine-teleop/turnserver.conf
```

渲染器原子写入并把输出权限固定为 `0600`，不会回显 secret。Compose 还要求把
`MINE_TELEOP_TURN_CONFIG_FILE` 指向该已渲染文件；未设置时会在启动前失败关闭，
避免把字面占位符当成生产 realm/secret。

归档 coturn `usage` 日志后，可以先在本地生成脱敏用量验收 JSONL：

```bash
scripts/coturn_usage_report.sh \
  --log /var/log/mine-teleop/coturn.log \
  --require-relay-bytes
```

该报告输出一条 `coturn_usage_report` 汇总记录和多条 `coturn_usage_sample`
样本记录，统计解析样本数、会话数、TURN client bytes 和 peer relay payload
bytes。报告只保留 session/actor 归属和包/字节计数，不回显 coturn 原始
username。coturn 4.6.2 的结束用量行没有可靠 duration，不能由该报告单独计算
平均带宽；真实带宽与云端账单对账仍需接入参与者 duration 指标及云厂商流量数据。

在有 Docker 的开发机上，可先运行真实的 Coturn 4.6.2 UDP/TCP 中继、短期凭据、
错误密码、匿名和过期凭据门禁：

```bash
scripts/check_coturn_relay.sh \
  --log-output /tmp/mine-teleop-coturn.log
```

Mac 使用 Colima 且宿主 Docker socket 不可用时，可以设置
`MINE_TELEOP_DOCKER_COMMAND="colima ssh -- docker"`。若要消费信令服务实际签发的
凭据，把 REST username 和 credential 分两行写入权限为 `0600` 的临时文件，
再传 `--credentials-file`；也可以分别放入 `MINE_TELEOP_TURN_USERNAME` 和
`MINE_TELEOP_TURN_CREDENTIAL` 环境变量。脚本输出不会回显二者。该门禁是隔离
Docker 网络内的真实分配和 relay payload 证明，不替代公网 NAT、TLS/DTLS 或
强制 WebRTC relay。

### Acceptance Metrics Report

直连、TURN UDP 中继或弱网测试结束后，把视频、控制、录像和上传采样保存为
JSONL，然后生成统一验收报告：

```bash
python3 scripts/acceptance_metrics_report.py \
  --samples /var/log/mine-teleop/acceptance-samples.jsonl \
  --scenario weak-100ms-turn-relay
```

输入样本按 `event` 分发给对应 recorder，例如 `video_sample`、
`control_receive`、`recording_segment` 和 `upload_sample`。输出第一行是
`acceptance_metrics_report` 汇总，后续按样本内容生成
`video_acceptance_metrics`、`control_acceptance_metrics`、
`recording_acceptance_metrics` 和 `upload_acceptance_metrics`。汇总和每条指标
报告都会输出顶层 `passed/failures`；录像分段或 sidecar 元数据不完整、上传样本
失败会显式标记 `passed=false`。该报告用于归档实测数据，不能替代目标环境的真实
时延、丢包、TURN 和车辆制动验证。
目标主机验证计划中的 `acceptance.metrics.report` 会要求上述 5 类输出齐全、
scenario 与 summary 一致，并拒绝任何显式失败的验收指标报告。

### 进程和日志

```bash
systemctl status mine-teleop-vehicle-agent
journalctl -u mine-teleop-vehicle-agent -f
```

### Signaling TLS

独立 C++ `mine-teleop-signaling-server` 默认绑定 `127.0.0.1`，只作为 TLS
反向代理的明文后端；没有显式开发开关时会拒绝非回环绑定。先用部署 secret
环境向进程注入驾驶员与设备凭据。可先执行
`mine-teleop-signaling-server --help` 核对完整参数；帮助和版本查询不会启动监听，
未知参数会直接失败，避免拼写错误后意外使用默认值。再启动回环后端：

```bash
MINE_TELEOP_DRIVER_PASSWORD='<from-secret-store>' \
MINE_TELEOP_DEVICE_TOKEN='<from-secret-store>' \
/opt/mine-teleop/bin/mine-teleop-signaling-server \
  --host 127.0.0.1 \
  --port 8765 \
  --driver-id driver-console-001 \
  --vehicle-id vehicle-001 \
  --login-max-failures 5 \
  --login-failure-window-ms 60000 \
  --login-lockout-ms 300000 \
  --api-rate-limit-requests 600 \
  --api-rate-limit-window-ms 60000 \
  --api-rate-limit-max-sources 4096 \
  --trusted-proxy-addresses 127.0.0.1,::1 \
  --audit-log-max-bytes 67108864 \
  --audit-log-files 5 \
  --audit-log-retention-days 7 \
  --audit-log /var/log/mine-teleop/signaling-audit.jsonl
```

上面的参数保留给单驾驶员/单车辆兼容模式。2×2 或更大部署必须改用身份配置文件；
secret 由服务管理器注入配置声明的环境变量，或存放在权限为 `0600` 的独立文件中：

```bash
/opt/mine-teleop/bin/mine-teleop-signaling-server \
  --config /etc/mine-teleop/signaling-server.yaml \
  --validate-config

/opt/mine-teleop/bin/mine-teleop-signaling-server \
  --config /etc/mine-teleop/signaling-server.yaml \
  --host 127.0.0.1 \
  --port 8765 \
  --audit-log /var/log/mine-teleop/signaling-audit.jsonl
```

校验输出只包含 `driver_count`、`vehicle_count` 和 `permission_count`，不会打印 ID
或 secret；该模式拒绝重复 ID、空白名单、未知车辆、缺失/双重 secret 来源以及和
旧单身份参数混用。仓库中的 `configs/signaling-server.2x2.dev.yaml` 是可运行结构示例。

`deployments/caddy/Caddyfile` 把 `/api/*`、原生 API 路径和 `/signaling/*`
转发到该回环后端，其它路径返回 404，因此公网不会出现驾驶页面。Caddy 的
`MINE_TELEOP_PUBLIC_ORIGIN` 必须是正式 HTTPS 域名，证书由 ACME 或部署环境的
可信证书流程提供；防火墙只开放 443，8765 不得公网监听。模板启动示例：

```bash
export MINE_TELEOP_PUBLIC_ORIGIN=https://teleop.example.com
export MINE_TELEOP_SIGNALING_UPSTREAM=127.0.0.1:8765
docker compose -f deployments/caddy/compose.yaml up -d
```

该 compose 使用 Linux host networking，使 Caddy 能访问宿主机回环后端；它不是
macOS Docker Desktop 模板。公网 443 只由 Caddy 监听，8765 仍保持回环。

当前拓扑只把回环 Caddy 视为可信直接上游。Caddy 默认丢弃来自非可信客户端的
`X-Forwarded-*` 值并为上游设置实际客户端来源，因此后端只在直接对端为
`127.0.0.1`/`::1` 时使用它提供的 `X-Forwarded-For`。不要把公网地址或宽泛网段
填入 `MINE_TELEOP_TRUSTED_PROXY_ADDRESSES`。如果以后在 Caddy 前增加 CDN/LB，必须
先单独设计并验收完整代理链，不能直接沿用当前单跳配置。

同一 NAT 出口下的多个控制端共享来源配额；正常容量应按现场并发留余量。出现
HTTP/WSS 429 时检查 `Retry-After`、`api_rate_limited` 审计事件，以及 `/health` 的
`api_rate_limit_tracked_sources`、`api_rate_limit_overflow_active` 和
`api_rate_limited_requests`。单进程重启会清空窗口，多实例之间也不共享状态。

`/health` 的 `alerts` 使用固定 `code`、`severity`、`count` 字段，不返回账号名或
来源 IP。当前支持 `login_lockout_active` 和
`api_rate_limit_source_capacity`；任一告警存在时顶层 `status=degraded`，窗口到期
后恢复 `status=ok`。`login_locked_buckets` 只统计当前仍在锁定期内的聚合桶，
`alert_count` 应与 `alerts` 数量一致。该接口适合本机探针和上游采集，但尚未实现
邮件、短信或值班系统投递。

信令服务为每个已处理的 HTTP 请求，以及每个命中信令路由的 WSS 握手生成
`request-` 前缀的 `X-Request-ID`；WSS 101、握手拒绝和 HTTP 2xx/4xx/429 都会返回
该字段。同一请求范围内写出的审计记录带相同的 `request_id`，可从客户端错误直接
定位审计行。服务端不会信任或回显客户端提交的 `X-Request-ID`；排障时应以响应头
为准。后台 reaper 等没有入站请求的事件仍依赖 `session_id`/对象 ID 关联。
每次服务运行还生成一个 `service-` 前缀的 `service_instance_id`，同一次运行的
`/health` 和全部审计记录保持一致，重启后更换；因此没有 `request_id` 的 reaper
事件仍可先按服务实例再按会话/对象关联。它不是主机身份，也不能跨重启当作稳定 ID。

`Caddyfile.local-wss` 的 `tls internal` 仅用于本机自动化验收。可导出内部根证书
并通过 `CURL_CA_BUNDLE` 验证客户端确实执行证书校验；不得把该根证书或内部
签发方式当作公网验收结果。

### Identity Credentials

内置 `dev-password`/`dev-device-secret` 只允许回环开发。部署时必须由 secret
manager 或权限收紧的 systemd `EnvironmentFile` 设置
`MINE_TELEOP_DRIVER_PASSWORD`、`MINE_TELEOP_DEVICE_TOKEN` 和可选 admin/TURN
secret，不能把值写进仓库、命令行、Caddy 配置或日志。当前 C++ 单机参考服务
仍使用内存账号和直接密码比较，尚未实现密码哈希、持久账号、外部 IAM 或设备
mTLS。账号级登录失败保护已启用：默认一分钟内第 5 次失败锁定五分钟，返回
`429`/`Retry-After`，并写入不含密码的失败和锁定审计；未知账号共用一个固定桶。
若合法操作员被锁定，应先从审计确认攻击或误输，不能靠重启服务作为常规解锁
流程。公网按来源/路由的 API 限流、持久限流状态和多实例共享仍是 S9 生产门禁，
不能用本机账号锁定或 TLS 验收替代。

### 车端启动前检查

```bash
python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --preflight \
  --hardware-device /dev/dri/renderD128 \
  --hardware-device /dev/dri/card1
```

该命令输出 JSONL：第一行是 `vehicle_preflight` 汇总，后续每行是一项
camera/recording/hardware 检查。全部 `ready` 或 `skipped` 时返回 0；存在
`missing`、`not_readable`、`not_writable` 或 `not_directory` 时返回 2。
`mine-teleop-vehicle-agent.service` 模板已把同一检查配置为第一条
`ExecStartPre`，所以缺少相机、录像目录或硬编设备时，长期控制循环不会启动。
第二条 `ExecStartPre` 会继续执行 `--adapter-status`，证明配置的
VehicleAdapter 能被打开后才进入 `--run-loop`。

### ChassisControl/MinePilot bridge 检查

```bash
python3 scripts/chassis_bridge_check.py \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --chassis-control-branch UI_Test \
  --minepilot-branch merge_ui_test \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --build-dir build/chassis-control-bridge \
  --build
```

该命令输出 JSONL：第一行是 `chassis_bridge_check` 汇总，后续每行是一项
ChassisControl 根目录、MinePilot `include/can/can_common.h`、
`include/can/can_message.h`、`can_db.h`、`can_receiver.h`、
`can_sender.h`、对应 `src/can_db.cpp`、`src/can_receiver.cpp`、
`src/can_sender.cpp`、`libchassis_control`、`chassis_control.symbols` ABI
符号导出、CMake configure 和可选
`cmake --build` 检查，对应 stdout 名称为 `cmake.configure` 和 `cmake.build`，
并确认 checkout 位于指定分支，同时记录 HEAD commit
和 dirty 状态。全部 `ready` 或 `skipped` 时返回 0；缺少库、头文件、
源文件、目录、`Initialize`、`UpdateVehicleState`、`SendCanMessage`、
`EmergencyStopWheels()` 等必需 ChassisControl 符号、CMake configure 或显式 `--build` 构建失败时返回 2。macOS
开发机可以不加 `--build` 验证路径和 configure；目标 CAN 主机验收应保留
`--build`，证明 bridge target 能链接所选 ChassisControl/MinePilot 动态库。
`--skip-cmake` 不能和 `--build` 混用；请求 build 时如果跳过 configure，
`cmake.build` 会标记为 failed，避免把未执行的链接验证误当作目标主机验收。
归档报告会要求 `cmake.configure` 和 `cmake.build` 的 stdout `path` 均匹配
summary `bridge_build_dir`，`chassis_control.library` 的 stdout `path`
必须位于 ChassisControl 或 MinePilot checkout，`chassis_control.symbols`
必须用同一动态库 path 完成符号校验，并核对 `chassis_bridge_check` summary `check_count`
与实际检查记录数量一致，避免把其它构建目录或截断/拼接的 stdout 误当作 bridge
build 证据。
如果在 macOS 开发机上需要复现 Linux bridge 构建命令，可先生成 Docker 命令
计划；该模式只打印 JSONL，不会连接 Docker daemon：

```bash
python3 scripts/chassis_bridge_check.py \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so \
  --build-dir build/chassis-control-bridge \
  --docker-command
```

输出中的 `build_image_command` 使用 MinePilot 的 Ubuntu 22.04 Dockerfile 构建
`linux/amd64` 镜像；`run_command` 会把 mine-teleop、ChassisControl 和 MinePilot
挂载到 `/workspace`，把显式 `--chassis-control-library` 映射为容器内
`-DCHASSIS_CONTROL_LIBRARY=/workspace/MinePilot/libchassis_control.so`，并在容器内
执行 bridge CMake configure/build。

### VehicleAdapter 状态 smoke

```bash
python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status
```

该命令先输出脱敏后的 `effective_vehicle_config`，随后创建并打开配置中的
VehicleAdapter，输出 `vehicle_adapter_status` JSON。`ready=true` 要求
`status.opened=true` 且 `status.healthy=true`；打开动态库、CAN interface 或
C shim 失败时返回 2，并在 `status.last_error` 中记录失败原因。目标主机验证
计划把该 smoke 列为 required，用来证明 bridge 不是只完成构建，而是能被
vehicle-agent runtime 实际打开。归档报告还会校验 `status.library_path`
与 summary `bridge_library_path` 一致；summary `chassis_control_library_path`
保留该 shim 链接的底层 ChassisControl 动态库路径。

### VehicleAdapter feedback poll

```bash
python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status \
  --poll-feedback \
  --require-feedback
```

该命令必须在同一 stdout JSONL 中输出 `vehicle_adapter_status` 和
`vehicle_adapter_feedback_poll`。归档报告会复用 adapter status 规则，要求
adapter 类型为 `can` 或 `dynamic_library`、`opened=true`、`healthy=true`、
`ready=true`、CAN interface 与 summary `can_interface` 一致，library path
与 summary `bridge_library_path` 一致，并且
feedback poll 的 `received=true` 且 snapshot 包含 decoded CAN 字段。

### MinePilot CAN 收发探针

目标主机验证计划会额外生成 MinePilot CAN 验收命令：

- `minepilot.can.sources`：确认低层 `include/can/can_common.h`、
  `include/can/can_message.h`、`can_db.h/.cpp`、`can_receiver.h/.cpp` 和
  `can_sender.h/.cpp` 均来自目标 MinePilot checkout；归档报告会校验 stdout
  `root` 与 summary `minepilot_root` 一致，避免把其它 MinePilot checkout 的
  CAN 源码清单误当作目标主机证据。
- `minepilot.can.socket.probe`：运行 MinePilot `script/check_can.sh <interface>`，
  检查 CAN kernel module、interface、统计信息和 raw socket bind；归档报告会
  校验 stdout `script=<minepilot_root>/script/check_can.sh`，以及 stdout
  `interface` 与 summary `can_interface` 一致。
- `minepilot.can.sender.build`：以 `-DBUILD_TESTING=ON` 构建
  `can_sender_main`，确保发送探针链接 `can_sender`、`can_receiver` 和
  `can_db`。归档报告会要求 stdout `target=can_sender_main` 且 `build_dir`
  与 summary `minepilot_can_probe_build_dir` 一致，避免把其它 MinePilot 测试目标
  或其它构建目录误当作发送探针构建证据。
- `minepilot.can.sender.smoke`：限时运行 `can_sender_main <interface>`；
  退出码 `124` 代表 `timeout` 按预期结束长运行发送探针，接口打开失败等其它
  非 0 退出码必须视为验收失败。归档报告会同时要求 accepted exit code 和
  stdout `interface` 与 summary `can_interface` 一致；同时校验 stdout
  `executable=<minepilot_can_probe_build_dir>/can_sender_main` 和 `timeout_seconds`
  分别匹配 summary `minepilot_can_probe_build_dir`、`can_probe_timeout_seconds`，
  且 `startup_banner_seen=true`，证明 `can_sender_main` 已越过 CAN socket
  打开并启动 MinePilot 发送线程。

## 已知环境问题

### `/usr/local/bin/ffmpeg` VAAPI 崩溃

症状：

```text
libva.so.2: undefined symbol: vaMapBuffer2
```

原因：

- `/usr/local/bin/ffmpeg` 是很新的 FFmpeg 构建。
- 宿主机 Ubuntu 22.04 的 `libva2 2.14.0` 较旧。
- ABI 不匹配导致崩溃。

处理：

- 不用该二进制判断硬件能力。
- 使用 apt 版本 FFmpeg 或容器中匹配版本。

### apt/ROS broken

症状：

```text
python3-catkin-pkg-modules trying to overwrite catkin_pkg/__init__.py
```

原因：

- ROS2 源和 Ubuntu 源中的 catkin 相关包版本/拆包方式冲突。
- 多个 ROS Humble 包处于未完成配置状态。

处理：

- 不要随手 `dpkg --force-overwrite`。
- 不要先 `autoremove`。
- 如果只是验证硬编，优先 Docker。
- 真要修 apt，先做 dry-run 并确认不会卸载关键 ROS 包。

### Docker 拉镜像失败

症状：

```text
failed to resolve reference docker.io/library/ubuntu:22.04
```

处理：

- 给 Docker daemon 配置代理。
- 或使用可信镜像代理后重新 tag。

Snap Docker 的代理配置可能不同于 snapd 自身代理，需要确认 Docker daemon 服务名。

## 日志字段建议

所有组件日志建议包含：

- `ts`
- `level`
- `component`
- `vehicle_id`
- `session_id`
- `camera_id`
- `event`
- `message`
- `error_code`

本地参考实现提供 `ComponentLog`，按上述字段写入 JSONL，并可按大小轮转为
`.1`、`.2` 等编号备份文件，便于长期运行时接入文件日志。组件日志同时支持
`logging.level` 运行时更新，低于当前最小级别的记录不会写入文件。

C++ 信令服务的 `--audit-log` 追加 UTC JSONL；入站 HTTP/WSS 请求范围内的记录
还带服务端生成的 `request_id`。服务启动时先写
`signaling_service_started`。当前小时写入 `signaling-audit.jsonl`；小时切换时，
当前文件和小时内大小分片原子改名为
`signaling-audit.YYYYMMDDTHHMMSSZ.partNN.jsonl`，默认只保留最近 7 天。
所有检查、改名、追加和 flush 使用同一专用锁，多个请求线程不会并发操作
`ofstream` 或轮转文件。默认小时内单分片上限 64 MiB、最多 5 个活动分片；
`--audit-log-max-bytes`/`MINE_TELEOP_AUDIT_LOG_MAX_BYTES` 和
`--audit-log-files`/`MINE_TELEOP_AUDIT_LOG_FILES` 可调整大小边界；
`--audit-log-retention-days`/`MINE_TELEOP_AUDIT_LOG_RETENTION_DAYS` 调整 1 到
365 天保留期。轮转发生在完整 JSONL 记录写入前，不会拆分记录；审计目录缺失或
不可写会让服务启动失败。外部日志采集器应读取当前文件、当前 `.1` 等大小分片和
带 UTC 小时的归档，并保留顶层 `service_instance_id`；不要再由外部 logrotate
同时重命名这些文件。进程外可查询审计存储仍是 S8 待办。

关键事件：

- 配置加载。
- 编码器选择。
- 相机启动/失败。
- WebRTC ICE 状态变化。
- TURN 是否启用。
- 控制命令过期/乱序。
- 控制权授予/回收。
- 安全停车。
- 急停。
- 录像文件完成。
- 上传成功/失败。

## 监控指标

车端：

- CPU。
- GPU。
- 内存。
- 磁盘剩余。
- 磁盘写入速度。
- 5G 网络状态。
- 每路编码 fps。
- 每路实时码率。
- 控制命令频率。
- 控制超时次数。
- Adapter 类型、打开状态、健康状态、CAN 接口、动态库路径、控制下发计数、安全停车计数和最近错误。

云端：

- 信令连接数。
- TURN 中继流量。
- 会话数。
- 上传成功率。
- 上传失败原因。

驾驶端：

- 视频解码 fps。
- 控制发送频率。
- UI 卡顿。
- RTT。
- 丢包率。
