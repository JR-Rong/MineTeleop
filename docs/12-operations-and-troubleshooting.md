# 运维与排障

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

归档 coturn `usage` 日志后，可以先在本地生成脱敏用量验收 JSONL：

```bash
python3 scripts/coturn_usage_report.py \
  --log /var/log/mine-teleop/coturn.log
```

该报告输出一条 `coturn_usage_report` 汇总记录和多条 `coturn_usage_sample`
样本记录，统计解析样本数、忽略行数、会话数、relay bytes 累计量、duration
累计量和平均带宽。报告只保留 session/actor 归属和用量字段，不回显 coturn
原始 username；真实云端账单对账仍需在部署环境接入运营账单或云厂商流量数据。

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

`signaling-server --serve` 默认绑定 `127.0.0.1`，适合本机开发或由本机反向
代理转发。若直接绑定 `0.0.0.0`、公网 IP 或其它非回环地址，必须同时提供
`--tls-cert` 和 `--tls-key`：

```bash
python3 signaling-server/signaling_server.py \
  --serve \
  --host 0.0.0.0 \
  --port 8765 \
  --tls-cert /etc/mine-teleop/tls/signaling.crt \
  --tls-key /etc/mine-teleop/tls/signaling.key
```

如果使用 nginx、Caddy 或云负载均衡终止 TLS，推荐让 signaling server 继续
监听 `127.0.0.1`，公网入口只暴露 TLS 代理。

### Identity Credentials

驾驶员登录默认只适合本机开发。联调或部署时配置 PBKDF2-SHA256 驾驶员凭据文件：

```json
{
  "drivers": {
    "driver-console-001": {
      "algorithm": "pbkdf2_sha256",
      "iterations": 120000,
      "salt": "<base64-salt>",
      "digest": "<base64-pbkdf2-digest>"
    }
  }
}
```

车端设备 token 同样应通过文件显式配置：

```json
{
  "vehicles": {
    "vehicle-001": "<device-token>"
  }
}
```

然后启动服务时加载这些文件：

```bash
python3 signaling-server/signaling_server.py \
  --serve \
  --host 127.0.0.1 \
  --port 8765 \
  --driver-credentials /etc/mine-teleop/driver-credentials.json \
  --device-credentials /etc/mine-teleop/device-credentials.json
```

配置后，文件内驾驶员不再接受默认 `dev-password`，文件内车辆必须使用对应
`device_token`。真实生产账号生命周期、外部 IAM、设备证书和凭据轮换仍应由部署
环境接入。

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

本地参考实现的 `AuditLog` 使用同一编号备份轮转语义；启动
`signaling-server --serve` 时可配置 `--audit-log-max-bytes` 和
`--audit-log-backup-count`，让登录、会话、控制权、急停和上传等审计记录长期
落盘时也保持文件大小可控。

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
