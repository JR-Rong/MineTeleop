# 当前实现与工控机无 Docker 部署

> 迁移说明：本文保留旧实现的设计背景；当前可执行入口与命令以根目录 `README.md` 中的 Ubuntu 22.04 原生 C++ 运行时为准。

本文面向到工控机现场联调前的收束检查：先说明当前已经实现了什么，再把设计、实现、测试文档归类，最后给出不依赖 Docker 的部署与验收流程。

## 当前实现状态

已实现并有本地自动化测试覆盖：

- Python 参考栈：`mine_teleop/`、`vehicle-agent/`、`vehicle-media-agent/`、`vehicle-uploader/`、`driver-console/`、`signaling-server/`。
- 车端配置体系：YAML/TOML 加载、真实 adapter 启动门禁、公网 cloud URL 和设备凭据门禁、运行时可热更新字段白名单、最终生效配置脱敏日志。
- 控制协议与安全状态机：20 Hz 命令模型、序号和到达间隔校验、降级超时、控制超时制动、车端急停锁存和显式复位语义。
- 驾驶端参考逻辑：键盘/软件控件输入合成、失焦安全心跳、急停长按门控、状态栏/工具栏/视频面板状态模型、本地操作日志轮转。
- 媒体参考逻辑：TestPattern/FileReplay 源、V4L2 source 片段、低延迟 H.264 GStreamer/WebRTC pipeline 规划、H.264 profile 校验、pipeline watchdog、故障恢复决策。
- 录像与上传：独立录像分支规划、分段 sidecar 元数据、上传队列持久化、预签名 URL 刷新、上传限速、网络质量暂停、backlog 告警、本地 Upload API。
- 云端参考服务：健康检查、车辆上线、驾驶员登录、会话和控制权、HTTP JSON/WebSocket 信令队列、设备/驾驶员 token 校验、审计日志、TURN/ICE 发放和用量解析。
- 运维和验收工具：systemd 模板、容器模板、弱网 dry-run 计划、控制/视频/录像/上传验收指标汇总、目标主机验证计划和归档报告。
- ChassisControl/MinePilot 接入骨架：`deployments/chassis-control-bridge/` C shim、`scripts/chassis_bridge_check.py`、`scripts/render_chassis_vehicle_config.py`、`scripts/target_host_validation_plan.py`，用于链接 ChassisControl 动态库并通过 MinePilot `can_db`/`can_receiver`/`can_sender` 做 CAN 收发与 decoded feedback 拉取。

仍必须在工控机或台架验证：

- `libmine_teleop_chassis_bridge.so` 在目标 Linux/CAN 环境中的真实构建和加载。
- SocketCAN `can0` 绑定、MinePilot CAN socket/send smoke、decoded CAN feedback poll。
- ChassisControl 动态库真实控制量单位、档位、心跳、安全停车、急停触发/复位行为。
- 真实相机、GStreamer/VAAPI、4 路实时、4 路录像、实时+录像并发。
- 直连/TURN/弱网下的真实视频延迟、控制 RTT、上传影响和场地制动距离。
- 完整生产驾驶端 UI、原生 C++/Qt/GStreamer/WebRTC 产品化进程；当前仓库是先锁定安全语义和联调证据链的 Python 参考实现。

这些待验证项已经暴露到 `/etc/mine-teleop/vehicle-agent.yaml`，便于工控机现场通过改配置切换：

- `hardware.can.interface`、`hardware.can.bitrate`、`hardware.can.probe_timeout_seconds`
- `cameras[*].device`、`cameras[*].enabled`
- `hardware.encoding.vaapi_render_device`、`hardware.encoding.dri_card_device`、`hardware.encoding.gstreamer_*_plugins`
- `ice.turn_servers`
- `upload.backend=s3` 与 `upload.s3.*`
- `field_safety.*`、`control.timeout_calibration`、`control.estop`、`control.time_sync`

`vehicle-agent --preflight`、`vehicle-media-agent` 的硬编探测和 `target-host-validation-plan`
都会读取这些配置；命令行参数只用于临时覆盖。CAN interface 同时出现在 `hardware.can.interface`
和 `vehicle_adapter.integration.chassis_control.can_interface`，两者不一致时配置会被拒绝。

## 文档地图

设计文档：

- [00 项目背景](00-project-context.md)
- [01 需求规格](01-requirements.md)
- [02 系统架构](02-system-architecture.md)
- [03 车端 Agent 设计](03-vehicle-agent-design.md)
- [04 驾驶端 Console 设计](04-driver-console-design.md)
- [05 云端服务设计](05-cloud-services-design.md)
- [06 视频、录像与上传](06-media-recording-upload.md)
- [07 控制协议与安全停车](07-control-and-safety.md)
- [08 配置体系](08-configuration.md)
- [09 硬件与环境排查](09-hardware-and-environment.md)
- [ADR 0001 传输与媒体栈选择](adr/0001-transport-and-media-stack.md)
- [ADR 0002 硬件编码策略](adr/0002-hardware-encoding-strategy.md)

实现文档：

- [10 实施计划](10-implementation-plan.md)
- [12 运维与排障](12-operations-and-troubleshooting.md)
- [13 待确认问题](13-open-questions.md)
- [15 Ubuntu Bundle 软件说明](15-ubuntu-bundle-software.md)
- [16 Ubuntu Bundle 使用说明](16-ubuntu-bundle-usage.md)
- [17 Ubuntu Bundle 架构说明](17-ubuntu-bundle-architecture.md)
- [ChassisControl bridge](../deployments/chassis-control-bridge/README.md)
- [README 当前可运行实现](../README.md#当前可运行实现)

测试与验收文档：

- [11 测试与验收](11-testing-and-validation.md)
- [目标主机验证计划](12-operations-and-troubleshooting.md#目标主机验证计划)
- [ChassisControl/MinePilot bridge 检查](12-operations-and-troubleshooting.md#chassiscontrolminepilot-bridge-检查)
- [Acceptance Metrics Report](12-operations-and-troubleshooting.md#acceptance-metrics-report)

## 工控机部署目标形态

优先目标是不在工控机上安装 Docker，不依赖容器固定环境。当前最小可部署形态是：

```text
/opt/mine-teleop/
  app/                         # 本仓库代码
  lib/
    libchassis_control.so       # ChassisControl/MinePilot 产物
    libmine_teleop_chassis_bridge.so
  build/                        # 可选，现场构建 bridge 时使用
/etc/mine-teleop/
  vehicle-agent.yaml
/var/lib/mine-teleop/
  uploader/
/var/log/mine-teleop/
```

运行时不可避免的目标机条件：

- Linux 内核和 SocketCAN；现场 CAN 口已暴露为 `can0` 或配置中指定的接口。
- 与目标机 ABI 兼容的 `libchassis_control.so` 和 `libmine_teleop_chassis_bridge.so`。
- Python 3.9+ 和 `PyYAML`，用于当前参考实现入口。
- 如要测试媒体链路，还需要目标机已有 GStreamer/VAAPI/相机设备；只做 CAN/底盘 smoke 时可以先不跑媒体压测。

如果目标机连 Python/PyYAML 都不希望依赖，使用
[Ubuntu Bundle 使用说明](16-ubuntu-bundle-usage.md) 中的
`scripts/build_ubuntu_bundle.py`，在兼容 Linux 构建环境里用 PyInstaller 生成
`bin/mine-teleop` 单入口执行文件。工控机只需要执行文件、`lib/` 下动态库和配置。

## 部署前准备

在一台和工控机尽量一致的 Linux 构建机，或直接在工控机上准备三个目录：

```bash
sudo install -d \
  /opt/mine-teleop/app \
  /opt/mine-teleop/lib \
  /opt/mine-teleop/build \
  /etc/mine-teleop \
  /var/lib/mine-teleop/uploader \
  /var/log/mine-teleop
```

拷贝本仓库、ChassisControl 和 MinePilot：

```bash
rsync -a --delete --exclude .git --exclude build ./ /opt/mine-teleop/app/
rsync -a /path/to/ChassisControl/ /opt/ChassisControl/
rsync -a /path/to/MinePilot/ /opt/MinePilot/
```

确认外部分支或拷贝来源：

```bash
git -C /opt/ChassisControl branch --show-current
git -C /opt/MinePilot branch --show-current
```

默认集成基线是 ChassisControl `UI_Test`，MinePilot `merge_ui_test`。如果现场包没有 `.git`，需要在部署记录里写清楚来源 commit。

拷贝底层动态库：

```bash
sudo cp /opt/MinePilot/libchassis_control.so /opt/mine-teleop/lib/
```

如果实际库在 ChassisControl build 目录或其它路径，后续命令里的 `--chassis-control-library` 改成真实绝对路径。

## 构建或放置 C shim

推荐在目标 Linux/CAN 环境或 ABI 完全一致的构建机上构建：

```bash
cd /opt/mine-teleop/app
export LD_LIBRARY_PATH=/opt/mine-teleop/lib:${LD_LIBRARY_PATH:-}

python3 scripts/chassis_bridge_check.py \
  --chassis-control-root /opt/ChassisControl \
  --minepilot-root /opt/MinePilot \
  --chassis-control-branch UI_Test \
  --minepilot-branch merge_ui_test \
  --chassis-control-library /opt/mine-teleop/lib/libchassis_control.so \
  --build-dir /opt/mine-teleop/build/chassis-control-bridge \
  --build

sudo cp /opt/mine-teleop/build/chassis-control-bridge/libmine_teleop_chassis_bridge.so \
  /opt/mine-teleop/lib/
```

如果工控机没有 CMake/编译器，把上面命令放到兼容 Linux 构建机执行，然后只把两个 `.so` 拷到工控机的 `/opt/mine-teleop/lib/`。完整验收阶段仍建议临时保留 `/opt/ChassisControl` 和 `/opt/MinePilot`，因为验证计划会检查 CAN 源码、构建 MinePilot `can_sender_main`，并记录 checkout 证据。

## 生成车端配置

先按现场 CAN、cloud、相机、录像目录修改基准配置，再生成真实 adapter 配置：

```bash
cd /opt/mine-teleop/app
export LD_LIBRARY_PATH=/opt/mine-teleop/lib:${LD_LIBRARY_PATH:-}

sudo -E python3 scripts/render_chassis_vehicle_config.py \
  --base-config configs/vehicle-agent.dev.yaml \
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

`--max-control-timeout-ms` 和 `--calibration-evidence` 不能随便填。真实车辆联调前，必须用台架或封闭场地制动证据反推安全上限；当前示例值只展示命令形态。

生成后至少人工检查这些字段：

- `cloud.*` 是否指向现场 cloud/TURN。
- `device_cert`、`device_key` 或设备 token 是否按现场凭据配置。
- `cameras[*].device` 是否是目标机真实设备路径。
- `recording.root_dir` 是否可写。
- `vehicle_adapter.integration.chassis_control.library_path` 是否是 `/opt/mine-teleop/lib/libchassis_control.so`。
- `vehicle_adapter.integration.chassis_control.bridge_library_path` 是否是 `/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so`。

## CAN 和底盘 smoke

按现场 CAN bitrate 配置接口，下面只给命令形态：

```bash
sudo ip link set can0 down || true
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
ip -details link show can0
```

先做只读/打开类检查：

```bash
cd /opt/mine-teleop/app
export LD_LIBRARY_PATH=/opt/mine-teleop/lib:${LD_LIBRARY_PATH:-}

python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --preflight \
  --hardware-device /dev/dri/renderD128 \
  --hardware-device /dev/dri/card1

python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status

python3 vehicle-agent/vehicle_agent.py \
  --config /etc/mine-teleop/vehicle-agent.yaml \
  --adapter-status \
  --poll-feedback \
  --require-feedback
```

`--adapter-status --poll-feedback --require-feedback` 必须收到 decoded CAN feedback 才算通过。没有反馈帧时，不要进入真实控制测试。

不要在落地车辆上直接运行 `vehicle-agent --run-loop`。当前 `--run-loop` 是开发模拟入口，会生成固定示例控制命令；只允许在车轮离地、动力断开或其它安全台架条件下由现场安全员确认后使用。

## 目标主机完整验收

生成并执行现场验收脚本：

```bash
cd /opt/mine-teleop/app
export LD_LIBRARY_PATH=/opt/mine-teleop/lib:${LD_LIBRARY_PATH:-}

artifact_dir=/var/log/mine-teleop/target-validation-$(date +%Y%m%d-%H%M%S)

python3 scripts/target_host_validation_plan.py \
  --vehicle-config /etc/mine-teleop/vehicle-agent.yaml \
  --hardware-device /dev/dri/renderD128 \
  --hardware-device /dev/dri/card1 \
  --can-interface can0 \
  --network-interface wwan0 \
  --chassis-control-root /opt/ChassisControl \
  --minepilot-root /opt/MinePilot \
  --chassis-control-branch UI_Test \
  --minepilot-branch merge_ui_test \
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

脚本会继续执行每个检查，把 stdout/stderr/return code 写入 `artifact_dir`，并生成：

- `target_host_validation_results.jsonl`
- `target_host_validation_archive.jsonl`
- 每个检查对应的 `*.stdout.log` 和 `*.stderr.log`

如果需要复核归档：

```bash
python3 scripts/target_host_validation_report.py \
  --results "$artifact_dir/target_host_validation_results.jsonl" \
  --verify-artifacts
```

required 项失败、附件缺失、bridge summary 不一致、adapter 没打开、CAN feedback 没收到、uploader smoke 不是结构化 JSON，都会让归档报告失败。

## 现场测试顺序

建议按这个顺序推进，不要跳过安全边界：

1. 工控机文件和动态库就位：`ldd /opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so` 能找到底层库。
2. CAN 口只读检查：`ip -details link show can0`、MinePilot socket probe。
3. C shim 构建/加载检查：`scripts/chassis_bridge_check.py --build`。
4. 配置 preflight：相机、录像目录、DRI 节点、真实 adapter 配置。
5. `--adapter-status`：只证明 adapter 可打开且健康。
6. `--adapter-status --poll-feedback --require-feedback`：证明真实 decoded CAN feedback。
7. 车轮离地台架：验证控制量单位、档位、心跳、急停和复位。
8. 低速封闭场地：验证控制超时、分级制动、物理急停、安全员流程。
9. 媒体并发：4 路实时、4 路录像、实时+录像同时运行。
10. 网络验收：直连、TURN UDP 中继、弱网矩阵、上传影响。

## 带回的验收附件

现场测试结束后，把这些文件打包带回：

- `/etc/mine-teleop/vehicle-agent.yaml`，脱敏后归档。
- `target_host_validation_archive.jsonl`
- `target_host_validation_results.jsonl`
- `target-validation-*/*.stdout.log`
- `target-validation-*/*.stderr.log`
- `acceptance-samples.jsonl`
- `vehicle-media-agent --mode hardware-report` 输出。
- 控制、视频、录像、上传四类 acceptance metrics 输出。
- ChassisControl/MinePilot 的 commit、dirty 状态或源码包版本记录。

## 单文件可执行目标

当前仓库提供 `scripts/build_ubuntu_bundle.py`，用于生成“一个 Ubuntu 执行文件 +
两个核心 `.so`”的现场包。后续仍可继续增强：

- 增加更多目标发行版和 glibc 版本矩阵。
- 把 `libmine_teleop_chassis_bridge.so`、`libchassis_control.so` 和必要的系统动态库依赖列成 manifest。
- 用 `ldd` 和目标机 dry-run 验证动态库解析。
- 保留 `target_host_validation_plan.py` 或等价内置验收命令，避免打包后丢失现场证据链。

打包后的目标现场包是“`bin/mine-teleop` + `libmine_teleop_chassis_bridge.so` +
`libchassis_control.so` + 配置 + 验收脚本/文档”。如果临时不使用打包脚本，也仍可
回退到“仓库代码 + Python 3.9/PyYAML + 两个动态库 + 配置 + 验收脚本”的形态。
