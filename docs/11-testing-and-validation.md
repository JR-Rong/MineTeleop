# 测试与验收

本页只描述当前原生 C++ 运行时。构建入口以根目录 `README.md` 为准，已完成的
三机现场证据见 `docs/22-three-machine-live-delivery.md`，尚未完成的跨平台和
硬件门禁见 `docs/20-three-end-taskbook-status.md`。

## 验证层级

### 1. 静态检查

```bash
bash -n scripts/*.sh
```

同时检查文档和脚本是否引用已删除的文件。静态检查只能证明仓库内部一致性，
不能证明目标平台能够构建或运行。

### 2. Ubuntu 22.04 原生构建

```bash
scripts/check.sh
```

该 Docker 门禁会：

- 构建原生 C++20 目标；
- 运行 CTest；
- 校验开发配置；
- 执行确定性的控制超时安全停车；
- 检查信令、控制、媒体测试源和上传路径；
- 验证运行镜像不携带 Python。

任何 C++、CMake、配置或 Linux 打包改动都必须通过此门禁。

### 3. Ubuntu 车端成品

```bash
scripts/build_cpp_ubuntu_bundle.sh linux/amd64
```

构建脚本生成带 SHA-256 的自包含压缩包，并自动调用
`scripts/check_cpp_ubuntu_bundle.sh`。成品门禁校验架构、动态库、GStreamer
插件、配置、启动器和基础运行命令。

在目标车端解压后继续执行：

```bash
./bin/mine-teleop-run config-check --config config/vehicle-agent.yaml
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --preflight
./bin/mine-teleop-run vehicle-agent \
  --config config/vehicle-agent.yaml \
  --adapter-status
./bin/mine-teleop-run media-probe
```

`preflight` 必须确认启用的相机、CAN interface 和 bridge 动态库存在；
`adapter-status` 必须实际打开真实 adapter。两者失败时不得进入实车控制。

### 4. macOS 控制端

```bash
scripts/build_macos_control_bundle.sh
```

原生架构必须通过编译、CTest、签名、依赖、回环监听、端口冲突、页面脚本、
本地日志脱敏和退出清理。交叉编译的 x64 包只算 build-only，必须在 Intel Mac
或 Rosetta 环境补做运行验收。

双驾驶员、双车辆隔离门：

```bash
scripts/check_macos_control_2x2.sh /path/to/cmake-build
```

### 5. 控制面与 TURN

```bash
scripts/run_control_plane_docker_smoke.sh
scripts/check_coturn_relay.sh
```

控制面 smoke 验证信令、驾驶端页面和控制消息隔离。Coturn 门禁验证 UDP/TCP
relay、短期凭据、错误密码、匿名和过期凭据拒绝。二者仍不能替代公网 NAT 和
真实 WebRTC 强制 relay。

## 现场验收

### 视频

- 每路实际分辨率、fps、码率、丢包和解码失败次数；
- 端到端延迟和重连时间；
- 单路故障不能阻塞其它相机；
- 至少一次 30 分钟多路稳定性测试；
- NVENC 不可用时验证 VAAPI，H.265 不可用时验证 H.264。

### 控制与安全

- 正常链路维持 20 Hz；
- 乱序、过期、跨车辆和无权命令全部拒绝；
- 断网、浏览器退出、进程重启和 token 过期均触发安全状态；
- `control_timeout_ms` 必须由目标车辆、载重、坡度和路面制动数据标定；
- 急停解除必须经过现场物理确认。

### 录像与上传

- 分段可播放，sidecar 与文件校验和一致；
- 录像磁盘增长和保留策略符合配置；
- 上传失败可重试且不阻塞实时媒体；
- 上传限速下重新测量视频 fps、控制 RTT 和 CPU/IO。

### 底盘与 CAN

按 `deployments/chassis-control-bridge/README.md` 在 Ubuntu 22.04 构建 bridge，
然后验证：

- 真实 `libchassis_control.so` 与 bridge ABI 匹配；
- SocketCAN interface 可打开；
- MinePilot CAN receiver 能提供 decoded feedback；
- 缺失 feedback 时控制保持关闭；
- 安全停车和急停实际作用于目标底盘。

## 验收边界

本地或 Docker 全绿不等于现场通过。真实相机、GPU 驱动、CAN、底盘制动、
公网 STUN/TURN、Windows 控制端和 Ubuntu 控制端必须分别保留目标机证据。
