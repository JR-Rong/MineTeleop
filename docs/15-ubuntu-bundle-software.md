# Ubuntu Bundle 软件说明

本文说明交付给 Ubuntu 工控机的 Mine Teleop 软件包内容。该包的目标是让目标机不安装 Docker，不在现场拉 Python 依赖，只运行一个主执行文件和随包动态库。

## 产物结构

`scripts/build_ubuntu_bundle.py` 产出的目录形态：

```text
mine-teleop-ubuntu-x86_64/
  bin/
    mine-teleop
  lib/
    libmine_teleop_chassis_bridge.so
    libchassis_control.so
  configs/
    vehicle-agent.dev.yaml
    driver-console.dev.yaml
  docs/
    README.md
    14-current-status-and-ipc-deployment.md
    15-ubuntu-bundle-software.md
    16-ubuntu-bundle-usage.md
    17-ubuntu-bundle-architecture.md
  manifest/
    bundle_manifest.json
    file.txt
    ldd.txt
```

`bin/mine-teleop` 是 PyInstaller `--onefile` 生成的 Ubuntu x86_64 可执行文件。它内置当前 Python 参考实现和命令分发入口，不要求工控机安装 `PyYAML`。

`libmine_teleop_chassis_bridge.so` 是 Mine Teleop C shim，负责给 Python 侧提供稳定 C ABI。

`libchassis_control.so` 是底层 ChassisControl/MinePilot 动态库，负责真实底盘控制发送路径。

## 子命令

执行文件使用统一入口：

```bash
./bin/mine-teleop --list
```

主要子命令：

- `vehicle-agent`：车端 preflight、adapter status、CAN feedback poll 和开发控制循环入口。
- `vehicle-media-agent`：媒体 pipeline、GStreamer/VAAPI 探测计划和硬编验收报告。
- `vehicle-uploader`：录像上传队列 service smoke。
- `signaling-server`：本地 HTTP JSON/WebSocket 信令和 Upload API 参考服务。
- `driver-console`：驾驶端参考命令生成和操作日志。
- `chassis-bridge-check`：ChassisControl/MinePilot bridge 前置检查和构建检查。
- `render-chassis-vehicle-config`：生成真实 `vehicle-agent.yaml`。
- `target-host-validation-plan`：生成目标主机验收脚本。
- `target-host-validation-report`：复核目标主机验收归档。

## 运行边界

当前软件包用于工控机联调和验收证据收集，不代表完整生产 UI 已完成。真实 CAN、底盘控制量、急停、相机、VAAPI、TURN/S3 和场地制动仍必须在目标环境验证。

这些现场差异已经暴露到 `configs/vehicle-agent.dev.yaml` 和部署后的
`/etc/mine-teleop/vehicle-agent.yaml`：

- `hardware.can.*`：CAN interface、bitrate、probe 超时。
- `cameras[*]` 和 `hardware.encoding.*`：相机设备、VAAPI/DRI 节点、GStreamer 硬编/降级插件。
- `ice.turn_servers` 和 `upload.s3.*`：TURN 与 S3 目标、凭据文件。
- `field_safety.*`、`control.timeout_calibration`、`control.estop`：现场安全门禁和制动标定。

`vehicle-agent --preflight`、`vehicle-media-agent --mode vaapi-probe/gst-probe/hardware-probes` 和
`target-host-validation-plan` 都会读取这些配置；命令行参数只作为临时覆盖。

不要在落地车辆上直接运行 `bin/mine-teleop vehicle-agent --run-loop`。该模式会生成开发示例控制命令，只能在车轮离地、动力断开或其它现场安全条件确认后使用。

## 外部依赖

目标机仍需要：

- Linux x86_64 和 SocketCAN。
- 目标 CAN interface，例如 `can0`。
- 与随包动态库 ABI 兼容的 glibc/libstdc++。
- 如测试媒体链路，需要目标机已有相机设备、DRI/VAAPI 和 GStreamer/FFmpeg 工具。

工控机不需要 Docker。Docker 只用于构建机生成 `bin/mine-teleop` 和 bundle。
