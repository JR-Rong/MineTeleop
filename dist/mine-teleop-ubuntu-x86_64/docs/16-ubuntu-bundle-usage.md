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

脚本会把 `/Volumes/SystemDisk/Workspace` 下的仓库先复制到
`~/.cache/mine-teleop/ubuntu-bundle-workspaces/`，再挂载给 Docker。这样构建机可以
使用 Docker，工控机仍然不需要 Docker。

## 2. 拷贝到工控机

```bash
sudo install -d /opt/mine-teleop
sudo tar -xzf mine-teleop-ubuntu-x86_64.tar.gz -C /opt/mine-teleop --strip-components=1
sudo install -d /etc/mine-teleop /var/lib/mine-teleop/uploader /var/log/mine-teleop
```

设置动态库搜索路径：

```bash
export LD_LIBRARY_PATH=/opt/mine-teleop/lib:${LD_LIBRARY_PATH:-}
```

确认主执行文件和动态库：

```bash
/opt/mine-teleop/bin/mine-teleop --list
ldd /opt/mine-teleop/bin/mine-teleop
ldd /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so
```

## 3. 生成车端配置

首次联调用示例命令生成真实 adapter 配置：

```bash
/opt/mine-teleop/bin/mine-teleop render-chassis-vehicle-config \
  --base-config /opt/mine-teleop/configs/vehicle-agent.dev.yaml \
  --output /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-type can \
  --chassis-control-root /opt/ChassisControl \
  --minepilot-root /opt/MinePilot \
  --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so \
  --chassis-control-library /opt/mine-teleop/lib/libchassis_control.so \
  --can-interface can0 \
  --max-control-timeout-ms 900 \
  --calibration-evidence bench-brake-test-YYYY-MM-DD
```

`--max-control-timeout-ms` 和 `--calibration-evidence` 必须换成现场台架或封闭场地证据。

生成后先编辑 `/etc/mine-teleop/vehicle-agent.yaml`。现场差异都应在配置里表达：

- `hardware.can.interface`、`hardware.can.bitrate`、`hardware.can.probe_timeout_seconds`：CAN 口、bitrate 和 MinePilot CAN probe 超时。
- `vehicle_adapter.integration.chassis_control.*`：bridge 动态库、ChassisControl 动态库、CAN interface 和外部源码路径。
- `cameras[*].device`、`cameras[*].enabled`：真实相机设备和启用状态。
- `hardware.encoding.vaapi_render_device`、`hardware.encoding.dri_card_device`、`hardware.encoding.gstreamer_*_plugins`：VAAPI/DRI 节点和 GStreamer 硬编/降级插件。
- `ice.turn_servers`：STUN/TURN 地址、REST secret 或 credential 文件。
- `upload.backend=s3` 与 `upload.s3.*`：S3 endpoint、bucket、region 和凭据文件。
- `field_safety.*`、`control.timeout_calibration`、`control.estop`、`control.time_sync`：现场安全门禁、制动标定、急停复位和时间同步要求。

`render-chassis-vehicle-config --can-interface` 会同步写入 `hardware.can.interface` 和
`vehicle_adapter.integration.chassis_control.can_interface`。如果后续手动改 CAN 口，两处必须一致；
配置加载器会拒绝不一致的真实 adapter 配置。

## 4. CAN 和 adapter smoke

按配置中的 `hardware.can.interface` 和 `hardware.can.bitrate` 配置接口。下面以 `can0` 和
`500000` 为例：

```bash
sudo ip link set can0 down || true
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
ip -details link show can0
```

执行只读/打开检查：

```bash
/opt/mine-teleop/bin/mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --preflight

/opt/mine-teleop/bin/mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status

/opt/mine-teleop/bin/mine-teleop vehicle-agent \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status \
  --poll-feedback \
  --require-feedback
```

最后一个命令必须输出 `vehicle_adapter_feedback_poll` 且 `received=true`，否则不要进入真实控制测试。

## 5. 目标主机验收脚本

```bash
artifact_dir=/var/log/mine-teleop/target-validation-$(date +%Y%m%d-%H%M%S)

/opt/mine-teleop/bin/mine-teleop target-host-validation-plan \
  --vehicle-config /etc/mine-teleop/vehicle-agent.yaml \
  --chassis-control-root /opt/ChassisControl \
  --minepilot-root /opt/MinePilot \
  --bridge-library /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so \
  --chassis-control-library /opt/mine-teleop/lib/libchassis_control.so \
  --bridge-build-dir /opt/mine-teleop/build/chassis-control-bridge \
  --uploader-work-dir /var/lib/mine-teleop/uploader \
  --acceptance-samples /var/log/mine-teleop/acceptance-samples.jsonl \
  --acceptance-scenario target-host-acceptance \
  --artifact-dir "$artifact_dir" \
  --format shell > /tmp/mine-teleop-target-validation.sh

bash /tmp/mine-teleop-target-validation.sh
```

未显式传 `--hardware-device`、`--can-interface`、`--network-interface`、
`--can-probe-timeout-seconds` 时，`target-host-validation-plan` 会从
`/etc/mine-teleop/vehicle-agent.yaml` 的 `hardware.*` 字段读取。命令行参数仍可临时覆盖配置。

复核归档：

```bash
/opt/mine-teleop/bin/mine-teleop target-host-validation-report \
  --results "$artifact_dir/target_host_validation_results.jsonl" \
  --verify-artifacts
```

## 6. 带回文件

现场测试后带回：

- `/etc/mine-teleop/vehicle-agent.yaml`，脱敏后归档。
- `target_host_validation_results.jsonl`
- `target_host_validation_archive.jsonl`
- `*.stdout.log` 和 `*.stderr.log`
- `acceptance-samples.jsonl`
- `manifest/bundle_manifest.json`
- ChassisControl/MinePilot commit 或源码包版本记录。
