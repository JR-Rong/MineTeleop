# 实时遥操现场调试纪要

> 迁移说明：本文是历史调试记录；当前现场运行入口为 bundle 内的 `mine-teleop` C++ 可执行文件。

本文记录 2026-07-04 至 2026-07-05 这轮车端/控制端实时推流和控制反馈调试结论。
相关代码提交：`b9b8135 Improve live teleop media throughput`。

## 调试目标

现场验收关注三件事：

1. 车端多相机持续采集、编码并上传到本机 Docker 控制端。
2. 控制端网页实时显示车端图像、FPS 和端到端时延拆分。
3. 控制端键盘控制命令能回到车端，并在车端看到 JSONL 反馈日志。

## 主要问题与根因

### 帧率只有约 0.8 Hz

根因是实时预览路径每帧都会启动一次 `ffmpeg`：

- 车端每帧启动 `ffmpeg` 从 V4L2 采集并编码 H.264。
- 控制端每收到一帧 H.264 又启动一次 `ffmpeg` 解码成 PNG。

进程启动和 H.264 编解码成本远高于单帧 HTTP 传输成本，所以帧率被压到 0.x Hz。

修复方向：

- 车端新增 `MjpegFrameEncoder`，每个相机只启动一个持久 `ffmpeg` MJPEG pipe。
- 控制端新增 MJPEG/JPEG 直接接收路径，收到完整 JPEG 后直接作为 `image/jpeg` 输出到前端。
- `scripts/run_vehicle_live_media.sh` 默认使用 `MINE_TELEOP_FRAME_CODEC=mjpeg`。
- 仍保留 `MINE_TELEOP_FRAME_CODEC=h264` 作为旧路径回退。

### 只显示一个相机

根因是旧实时脚本只调用单相机检测逻辑，并只改写 live config 的第一路 camera。

修复方向：

- `scripts/run_vehicle_live_media.sh` 改为扫描所有 `Video Capture` 的 `/dev/video*`。
- 默认按 `front/rear/left/right/cameraN` 命名并生成完整 `cameras:` 配置块。
- 支持显式绑定：

```bash
MINE_TELEOP_CAMERA_DEVICES="front=/dev/video0 rear=/dev/video2" \
  scripts/run_vehicle_live_media.sh
```

### 车端看不到控制反馈

根因是本机控制端只通过 SSH 反向隧道暴露了网页/媒体入口 `8080 -> 18080`，没有把 signaling
服务 `8765` 暴露给车端。车端控制接收器无法轮询到控制命令。

修复方向：

- `scripts/start_live_control_plane_tunnel.sh` 同时建立：
  - `18080:127.0.0.1:8080`
  - `18765:127.0.0.1:8765`
- 输出车端 DataChannel 控制接受/拒绝与安全状态 JSONL。
- 本机 Docker runner 显式映射 `127.0.0.1:8765:8765`。

### 本机 Docker 控制端 signaling 启动失败

调试时发现把 signaling server 直接改为 `--host 0.0.0.0` 会触发安全门禁：

```text
--tls-cert and --tls-key are required for non-loopback hosts
```

修复方向：

- 保留生产默认安全策略：无 TLS 的非 loopback 绑定仍默认失败。
- 新增显式开发开关 `--allow-insecure-nonloopback-dev`，只在本机 Docker 现场预览脚本中使用。
- 该脚本仍只把端口绑定到宿主机 `127.0.0.1`，再通过 SSH 反向隧道给车端访问。

## 当前运行命令

### 本机启动控制端和隧道

```bash
cd /Users/rongjianrui/workspace/MineTeleop
MINE_TELEOP_VEHICLE_SSH_PASSWORD='******' \
  scripts/start_live_control_plane_tunnel.sh
```

成功后本机网页：

```text
http://127.0.0.1:8080/
```

车端可访问地址：

```text
http://127.0.0.1:18080/health
http://127.0.0.1:18765/health
```

### 车端部署最新包

本机已重新生成 Ubuntu bundle：

```text
dist/mine-teleop-ubuntu-x86_64.tar.gz
```

部署命令：

```bash
cd /Users/rongjianrui/workspace/MineTeleop
scripts/deploy_vehicle_bundle.sh
```

如果 SSH 需要额外参数，可追加：

```bash
scripts/deploy_vehicle_bundle.sh \
  --ssh-option ConnectTimeout=8 \
  --ssh-option PreferredAuthentications=password \
  --ssh-option PubkeyAuthentication=no
```

### 车端持续推流

自动启用所有相机：

```bash
cd /home/user/mine-teleop
scripts/run_vehicle_live_media.sh
```

显式指定多相机：

```bash
cd /home/user/mine-teleop
MINE_TELEOP_CAMERA_DEVICES="front=/dev/video0 rear=/dev/video2" \
  scripts/run_vehicle_live_media.sh
```

回退旧 H.264 单帧路径：

```bash
MINE_TELEOP_FRAME_CODEC=h264 scripts/run_vehicle_live_media.sh
```

### 车端控制反馈

`scripts/run_vehicle_live_media.sh` 启动的同一原生进程接收 WebRTC DataChannel 控制，
无需另开 HTTP 轮询控制进程。车端媒体 JSONL 日志应看到：

```text
seq
steering
throttle
brake
gear
sent_at_utc_ms
received_at_utc_ms
accepted
reason
```

## 前端观测字段

控制端网页相机卡片会显示：

- `fps`：控制端收到的实际帧率。
- `capture`：车端采集时间戳。
- `encode`：车端编码完成时间戳和编码耗时。
- `send`：车端发送时间戳。
- `receive`：控制端收到请求时间戳和传输耗时。
- `decode`：控制端解码完成时间戳和解码耗时。MJPEG 直接显示时该值为 `0ms`。
- `E2E`：采集到控制端可显示的端到端耗时。

同样的数据也在：

```text
http://127.0.0.1:8080/api/status
```

字段路径：

```text
latest_frame_timing_by_camera
decoded_frame_count_by_camera
status.bottom_bar.fps_by_camera
```

## 低照度调试结论

车端脚本默认启用温和低照度 profile：

```text
MINE_TELEOP_CAMERA_LOW_LIGHT=1
MINE_TELEOP_CAMERA_BRIGHTNESS=24
MINE_TELEOP_CAMERA_GAIN=96
MINE_TELEOP_CAMERA_GAMMA=450
MINE_TELEOP_CAMERA_BACKLIGHT=2
MINE_TELEOP_CAMERA_EXPOSURE_DYNAMIC_FRAMERATE=1
```

夜间仍偏暗时优先尝试降低采集 FPS 以换曝光时间：

```bash
MINE_TELEOP_CAPTURE_FPS=15 \
MINE_TELEOP_CAMERA_BRIGHTNESS=40 \
MINE_TELEOP_CAMERA_GAIN=120 \
MINE_TELEOP_CAMERA_EXPOSURE_ABSOLUTE=600 \
scripts/run_vehicle_live_media.sh
```

如果白天过曝：

```bash
MINE_TELEOP_CAMERA_LOW_LIGHT=0 scripts/run_vehicle_live_media.sh
```

## 已完成验证

本地验证结果：

```text
scripts/check.py
Ran 540 tests in 73.361s
OK
```

本地 Docker 控制端验证：

```text
http://127.0.0.1:8080/health -> ok
http://127.0.0.1:8765/health -> ok
```

本地 Docker 端口状态：

```text
mine-teleop-signaling-preview 127.0.0.1:8080->8080/tcp, 127.0.0.1:8765->8765/tcp
```

Ubuntu bundle 验证：

```text
dist/mine-teleop-ubuntu-x86_64.tar.gz
bin/mine-teleop.real
scripts/run_vehicle_live_media.sh includes MINE_TELEOP_FRAME_CODEC=mjpeg
```

## 当前阻塞点

调试机上远端端口检查结果：

```text
nc -vz -G 8 60.205.213.254 6000
Connection to 60.205.213.254 port 6000 succeeded
```

但 SSH 握手被远端关闭：

```text
ssh -p 6000 -o BatchMode=yes -o ConnectTimeout=8 user@60.205.213.254 true
Connection closed by 60.205.213.254 port 6000
```

即：TCP 端口通，SSH 服务或 FRP 后端在握手阶段关闭连接。因此本轮未完成车端部署和实车回传验证。
如果用户本机终端能登录，先执行部署命令，再按上面的本机隧道、车端推流和车端控制反馈命令继续验收。
