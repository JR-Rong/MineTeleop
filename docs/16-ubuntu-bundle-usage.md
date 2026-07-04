# Ubuntu Bundle 使用说明

本文给出从构建机打包到工控机现场 smoke 的命令流程。工控机不需要 Docker。

## 1. 在构建机生成包

构建机可以是本机 macOS + Docker，也可以是 Ubuntu x86_64 构建机。默认使用已有 `minepilot-build-env` 镜像：

```bash
python3 scripts/build_ubuntu_bundle.py \
  --output-dir dist/mine-teleop-ubuntu-x86_64 \
  --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl \
  --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot \
  --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so
```

成功后得到：

```text
dist/mine-teleop-ubuntu-x86_64/
dist/mine-teleop-ubuntu-x86_64.tar.gz
```

如果只想看 Docker 构建命令，不执行：

```bash
python3 scripts/build_ubuntu_bundle.py --dry-run
```

如果当前验收只覆盖媒体、控制信令、录像上传，不覆盖真实 CAN 收发，可显式跳过 C shim bridge
构建。脚本会生成一个 no-CAN stub `libmine_teleop_chassis_bridge.so`，保证车端包仍然是“执行文件
+ 随包动态库”的完整形态；真实 CAN 联调时再去掉 `--skip-bridge-build` 并使用完整
ChassisControl/MinePilot bridge：

```bash
python3 scripts/build_ubuntu_bundle.py \
  --output-dir dist/mine-teleop-ubuntu-x86_64 \
  --chassis-control-root /Users/rongjianrui/workspace/ChassisControl \
  --minepilot-root /Users/rongjianrui/workspace/MinePilot \
  --chassis-control-library /Users/rongjianrui/workspace/MinePilot/libchassis_control.so \
  --skip-bridge-build
```

脚本会把 `/Volumes/SystemDisk/Workspace` 下的仓库先复制到
`~/.cache/mine-teleop/ubuntu-bundle-workspaces/`，再挂载给 Docker。这样构建机可以
使用 Docker，工控机仍然不需要 Docker。

## 2. 通过 SSH 部署到工控机

如果目标车端已经通过 FRP 暴露 SSH，可直接把 bundle 传到目标用户目录并执行随包 smoke。
默认目标就是现场联调隧道 `ssh -p 6000 user@60.205.213.254`：

```bash
scripts/deploy_vehicle_bundle.sh
```

脚本只使用 `ssh`、`scp` 和远端随包文件，不会在工控机上运行 Docker。首次执行前可以先 dry-run：

```bash
scripts/deploy_vehicle_bundle.sh --dry-run
```

如果本机 Docker 控制端有一个车端可访问的 HTTP 地址，可让车端部署后立即发送测试 H.264
帧到控制端：

```bash
scripts/deploy_vehicle_bundle.sh \
  --driver-console-url http://CONTROL_HOST:8080 \
  --media-frames 3
```

如果控制端信令服务也有车端可访问的 HTTP 地址，可让车端进入控制接收模式，并把接收到的控制
命令逐条打印成 JSONL 日志：

```bash
scripts/deploy_vehicle_bundle.sh \
  --signaling-http-url http://CONTROL_HOST:8765 \
  --run-control-teleop
```

可用 `--ssh-option BatchMode=yes`、`--ssh-option ConnectTimeout=8` 等参数传递额外 SSH 选项。

## 2.1 本机 Docker 控制端和车端实时推流

本机控制端通过 Docker 运行，车端通过 SSH 反向隧道访问本机 `8080`。先在本机启动控制端和
反向隧道；脚本会在本机检查 `/health`，并从车端反查 `http://127.0.0.1:18080/health`，避免车端
脚本启动后刷 `Connection refused`：

```bash
cd /Users/rongjianrui/workspace/MineTeleop
MINE_TELEOP_VEHICLE_SSH_PASSWORD='******' \
  scripts/start_live_control_plane_tunnel.sh
```

再在车端运行实时媒体脚本。脚本会自动选择第一个支持 `Video Capture` 的 `/dev/video*`，优先选择
支持 MJPEG 的节点，生成 `configs/vehicle-agent.live.yaml`，持续采集、H.264 编码并 POST 到控制端：

```bash
cd /home/user/mine-teleop
scripts/run_vehicle_live_media.sh
```

前端相机卡片会显示最新帧的细分时序：

- `capture`：车端采集开始时间戳。
- `encode`：车端编码完成时间戳和采集到编码耗时。
- `send`：车端发送时间戳。
- `receive`：控制端收到请求时间戳和传输耗时。
- `decode`：控制端解码完成时间戳和解码耗时。
- `E2E`：采集到解码完成的端到端耗时。

这些字段也会出现在 `/api/status` 的 `latest_frame_timing_by_camera` 中。严格时延验收要求车端和
控制端时钟同步；如果两端时钟存在几十毫秒偏差，`send` 可能看起来晚于 `receive`，脚本会把传输耗时
钳制为 `0ms`，但端到端分析仍应先修正 NTP/chrony。

夜间或低照度时，车端脚本默认启用一个温和的低照度 profile：

```text
MINE_TELEOP_CAMERA_LOW_LIGHT=1
MINE_TELEOP_CAMERA_BRIGHTNESS=24
MINE_TELEOP_CAMERA_GAIN=96
MINE_TELEOP_CAMERA_GAMMA=450
MINE_TELEOP_CAMERA_BACKLIGHT=2
MINE_TELEOP_CAMERA_EXPOSURE_DYNAMIC_FRAMERATE=1
```

如果白天过曝，可关闭低照度增强：

```bash
MINE_TELEOP_CAMERA_LOW_LIGHT=0 scripts/run_vehicle_live_media.sh
```

如果夜间仍偏暗，可以降低采集帧率让相机有更长曝光时间，或者切到手动曝光：

```bash
MINE_TELEOP_CAPTURE_FPS=15 \
MINE_TELEOP_CAMERA_BRIGHTNESS=40 \
MINE_TELEOP_CAMERA_GAIN=120 \
MINE_TELEOP_CAMERA_EXPOSURE_ABSOLUTE=600 \
scripts/run_vehicle_live_media.sh
```

## 3. 手动拷贝到工控机主目录

工控机端不安装 systemd 服务，不设置自动启动；所有文件放在普通用户主目录下，手动执行。

```bash
base="$HOME/mine-teleop"
mkdir -p "$base" "$base/etc" "$base/logs" \
  "$base/data/recordings" "$base/data/uploader" "$base/data/uploader-archive" \
  "$base/deps"
tar -xzf mine-teleop-ubuntu-x86_64.tar.gz -C "$base" --strip-components=1
tar -xzf ChassisControl-source-ui-test.tar.gz -C "$base/deps"
tar -xzf MinePilot-source-merge-ui-test.tar.gz -C "$base/deps"
```

确认主执行文件和随包媒体工具，不依赖宿主机 `/usr/local/bin/ffmpeg`：

```bash
"$base/bin/mine-teleop" --list
"$base/bin/ffmpeg" -hide_banner -hwaccels
"$base/bin/vainfo" --display drm --device /dev/dri/renderD128
file "$base/bin/mine-teleop.real"
ldd "$base/bin/mine-teleop.real"
ldd "$base/lib/libmine_teleop_chassis_bridge.so"
```

包根目录还包含一个手动 smoke 入口，不安装服务、不设置自动启动，适合现场先验证
主程序、相机、CAN 只读状态、adapter 打开状态和 loopback 信令：

```bash
cd "$HOME/mine-teleop"
./manual-smoke.sh
```

默认检查 `can1`、`/dev/video0`、`/dev/video2` 和 `/dev/dri/renderD128`。如果现场设备名不同，
用环境变量覆盖：

```bash
MINE_TELEOP_CAN_IFACE=can0 \
MINE_TELEOP_CAMERAS="/dev/video4 /dev/video6" \
MINE_TELEOP_VAAPI_DEVICE=/dev/dri/renderD128 \
MINE_TELEOP_FEEDBACK_ATTEMPTS=10 \
./manual-smoke.sh
```

脚本不会执行 `vehicle-agent --run-loop`，也不会运行包含 `can_sender_main` 的全量
`target-validation.sh`。如果 `adapter-feedback` 只返回 `received=false`，说明 CAN 口和 adapter
可打开，但真实底盘反馈帧尚未按当前协议读到，仍不能进入真实控制测试。

## 4. 生成车端配置

首次联调用示例命令生成真实 adapter 配置：

```bash
base="$HOME/mine-teleop"
can_iface=can1

"$base/bin/mine-teleop" render-chassis-vehicle-config \
  --base-config "$base/configs/vehicle-agent.dev.yaml" \
  --output "$base/etc/vehicle-agent.yaml" \
  --adapter-type can \
  --chassis-control-root "$base/deps/ChassisControl" \
  --minepilot-root "$base/deps/MinePilot" \
  --bridge-library "$base/lib/libmine_teleop_chassis_bridge.so" \
  --chassis-control-library "$base/lib/libchassis_control.so" \
  --can-interface "$can_iface" \
  --recording-root "$base/data/recordings" \
  --network-interface wlx6c1ff77d6624 \
  --ffmpeg-binary "$base/bin/ffmpeg" \
  --ffprobe-binary "$base/bin/ffprobe" \
  --vainfo-binary "$base/bin/vainfo" \
  --libva-drivers-path "$base/lib/dri" \
  --camera-device front=/dev/video0 \
  --camera-device rear=/dev/video2 \
  --camera-capture-size 1280x720 \
  --camera-capture-fps 30 \
  --max-control-timeout-ms 900 \
  --calibration-evidence ipc-smoke-no-motion-YYYY-MM-DD
```

`--max-control-timeout-ms` 和 `--calibration-evidence` 必须换成现场台架或封闭场地证据。

生成后先编辑 `$HOME/mine-teleop/etc/vehicle-agent.yaml`。现场差异都应在配置里表达：

- `hardware.can.interface`、`hardware.can.bitrate`、`hardware.can.probe_timeout_seconds`：CAN 口、bitrate 和 MinePilot CAN probe 超时。
- `vehicle_adapter.integration.chassis_control.*`：bridge 动态库、ChassisControl 动态库、CAN interface 和外部源码路径。
- `cameras[*].device`、`cameras[*].enabled`：真实相机设备和启用状态。
- `hardware.encoding.vaapi_render_device`、`hardware.encoding.dri_card_device`：VAAPI/DRI 节点。
- `hardware.encoding.ffmpeg_binary`、`hardware.encoding.ffprobe_binary`、`hardware.encoding.vainfo_binary`、`hardware.encoding.libva_drivers_path`：随包媒体工具和 VAAPI driver 路径，工控机手动部署应指向 `$HOME/mine-teleop/bin/*` 和 `$HOME/mine-teleop/lib/dri`。
- `ice.turn_servers`：STUN/TURN 地址、REST secret 或 credential 文件。
- `upload.backend=s3` 与 `upload.s3.*`：S3 endpoint、bucket、region 和凭据文件。
- `field_safety.*`、`control.timeout_calibration`、`control.estop`、`control.time_sync`：现场安全门禁、制动标定、急停复位和时间同步要求。

`render-chassis-vehicle-config --can-interface` 会同步写入 `hardware.can.interface` 和
`vehicle_adapter.integration.chassis_control.can_interface`。如果后续手动改 CAN 口，两处必须一致；
配置加载器会拒绝不一致的真实 adapter 配置。

## 5. CAN 和 adapter smoke

按配置中的 `hardware.can.interface` 和 `hardware.can.bitrate` 配置接口。下面以 `can1` 和
`500000` 为例：

```bash
sudo ip link set can1 down || true
sudo ip link set can1 type can bitrate 500000
sudo ip link set can1 up
ip -details link show can1
```

执行只读/打开检查：

```bash
base="$HOME/mine-teleop"

"$base/bin/mine-teleop" vehicle-agent \
  --config "$base/etc/vehicle-agent.yaml" \
  --preflight

"$base/bin/mine-teleop" vehicle-agent \
  --config "$base/etc/vehicle-agent.yaml" \
  --adapter-status

"$base/bin/mine-teleop" vehicle-agent \
  --config "$base/etc/vehicle-agent.yaml" \
  --adapter-status \
  --poll-feedback
```

最后一个命令如果输出 `vehicle_adapter_feedback_poll` 且 `received=true`，说明底盘反馈链路已经可读；
否则不要进入真实控制测试。不要在未确认安全窗口时执行 `vehicle-agent --run-loop` 或目标脚本里的
`can_sender_main` smoke。

摄像头和硬编 smoke 使用随包 ffmpeg：

```bash
base="$HOME/mine-teleop"
"$base/bin/ffmpeg" -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
  -video_size 1280x720 -framerate 30 -i /dev/video0 -t 3 -f null -
"$base/bin/ffmpeg" -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
  -video_size 1280x720 -framerate 30 -i /dev/video2 -t 3 -f null -

mkdir -p "$base/data/vaapi-smoke"
"$base/bin/ffmpeg" -hide_banner -loglevel error -y \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 \
  -vf format=nv12,hwupload -c:v h264_vaapi -b:v 4M \
  "$base/data/vaapi-smoke/test.mp4"
"$base/bin/ffprobe" -hide_banner -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate \
  -of default=nw=1 "$base/data/vaapi-smoke/test.mp4"
```

## 6. 目标主机验收脚本

```bash
base="$HOME/mine-teleop"
artifact_dir="$base/logs/target-validation-$(date +%Y%m%d-%H%M%S)"

"$base/bin/mine-teleop" target-host-validation-plan \
  --vehicle-config "$base/etc/vehicle-agent.yaml" \
  --mine-teleop-binary "$base/bin/mine-teleop" \
  --ffmpeg-binary "$base/bin/ffmpeg" \
  --ffprobe-binary "$base/bin/ffprobe" \
  --vainfo-binary "$base/bin/vainfo" \
  --libva-drivers-path "$base/lib/dri" \
  --chassis-control-root "$base/deps/ChassisControl" \
  --minepilot-root "$base/deps/MinePilot" \
  --bridge-library "$base/lib/libmine_teleop_chassis_bridge.so" \
  --chassis-control-library "$base/lib/libchassis_control.so" \
  --bridge-build-dir "$base/build/chassis-control-bridge" \
  --uploader-work-dir "$base/data/uploader" \
  --acceptance-samples "$base/logs/acceptance-samples.jsonl" \
  --acceptance-scenario target-host-acceptance \
  --artifact-dir "$artifact_dir" \
  --format shell > "$base/target-validation.sh"

grep -E '/usr/local/bin/ffmpeg|sudo docker|apt-get' "$base/target-validation.sh" && exit 1 || true
```

确认现场安全、底盘支撑和 CAN 发送窗口后，再手动执行 `$base/target-validation.sh`。该脚本包含
MinePilot CAN sender smoke，不应作为首次无看护启动命令。

未显式传 `--hardware-device`、`--can-interface`、`--network-interface`、
`--can-probe-timeout-seconds` 时，`target-host-validation-plan` 会从
`$HOME/mine-teleop/etc/vehicle-agent.yaml` 的 `hardware.*` 字段读取。命令行参数仍可临时覆盖配置。

复核归档：

```bash
"$base/bin/mine-teleop" target-host-validation-report \
  --results "$artifact_dir/target_host_validation_results.jsonl" \
  --verify-artifacts
```

## 6. 带回文件

现场测试后带回：

- `$HOME/mine-teleop/etc/vehicle-agent.yaml`，脱敏后归档。
- `target_host_validation_results.jsonl`
- `target_host_validation_archive.jsonl`
- `*.stdout.log` 和 `*.stderr.log`
- `acceptance-samples.jsonl`
- `manifest/bundle_manifest.json`
- ChassisControl/MinePilot commit 或源码包版本记录。
