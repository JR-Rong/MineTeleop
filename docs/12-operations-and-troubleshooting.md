# 运维与排障

## 运行方式

车端建议最终以 systemd 或容器方式运行。

systemd 目标：

- 开机自启动。
- 异常退出自动重启。
- 日志进入 journald 和文件。
- 环境变量固定。

容器目标：

- 固定媒体栈版本。
- 挂载 `/dev/dri`。
- 挂载相机设备。
- 挂载录像目录。
- 限制资源。

## 关键检查命令

### GPU 和编码能力

```bash
lspci -nnk | grep -EA4 'VGA|3D|Display|NVIDIA|Intel|AMD'
ls -l /dev/dri
nvidia-smi || true
vainfo --display drm --device /dev/dri/renderD128
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -encoders | grep -Ei 'vaapi|qsv|nvenc|x264'
gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc
```

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

### 进程和日志

```bash
systemctl status mine-teleop-vehicle-agent
journalctl -u mine-teleop-vehicle-agent -f
```

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

关键事件：

- 配置加载。
- 编码器选择。
- 相机启动/失败。
- WebRTC ICE 状态变化。
- TURN 是否启用。
- 控制命令过期/乱序。
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

