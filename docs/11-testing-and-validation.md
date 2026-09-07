# 测试与验收

本页只描述当前原生 C++ 运行时。构建入口以根目录 `README.md` 为准，已完成的
三机现场证据见 `docs/22-three-machine-live-delivery.md`，尚未完成的跨平台和
硬件门禁见 `docs/20-three-end-taskbook-status.md`。

## 验证层级

### 1. 静态检查

```bash
find scripts -type f -name '*.sh' -print0 \
  | xargs -0 -n1 bash -n
```

同时检查文档和脚本是否引用已删除的文件。静态检查只能证明仓库内部一致性，
不能证明目标平台能够构建或运行。

### 增量开发质量检查

新增或改动 C++、Shell、浏览器 JavaScript、CommonJS 测试或协议工具时，先以当前
PR 的起点作为明确基线运行只读检查：

```bash
scripts/test/check_incremental_quality.sh --base <pr-head-sha>
```

该脚本只检查基线之后，以及 staged、unstaged 和未跟踪的相关文件；不会执行
`clang-format -i`、`clang-tidy --fix` 或全仓格式化。C++ 格式使用仓库根目录的
`.clang-format`，固定为 clang-format 18；`.clang-tidy` 只启用一组小的
bugprone/performance 检查，且仅在显式提供 build 目录时运行：

```bash
scripts/test/check_incremental_quality.sh --base <pr-head-sha> --compile-commands /path/to/cmake-build
```

浏览器脚本和 `.cjs` 测试共享 `.eslint.config.cjs`，但 ESLint 是可选的本地开发
工具；未安装时脚本仍会运行 `node --check` 并明确报告 skip。需要把某一环境变成
阻塞门禁时，添加 `--require-format`、`--require-eslint` 或 `--require-tidy`。当前
质量层不引入全仓历史 baseline、自动修复或新的测试框架。

GitHub Actions 中独立的 `Linux incremental quality` job 是目前的阻塞环境：它以
完整 Git history checkout，PR 使用 `pull_request.base.sha` 作为基线；向 `main`
的非 PR push 使用该 push 的 `before` SHA。两者都会先确认基线对象存在且是当前
HEAD 的祖先，因此首个零 SHA push 或 force push 的非祖先基线会明确失败，而不会
悄悄改用错误的比较范围。手动 dispatch 使用当前 HEAD 的第一父提交。

该 job 固定 Node 20.19.5、锁文件中的 ESLint 9.39.5，以及 LLVM APT 的
`clang-format-18` 18.1.8 精确包版本；通过 `npm ci` 安装 lint 依赖，且仅以
`--dry-run --Werror` 的 formatter 检查运行新增/改动文件。它调用：

```bash
scripts/test/check_incremental_quality.sh \
  --base <event-base-sha> \
  --require-format \
  --require-eslint
```

`clang-tidy` 尚未成为该轻量 job 的必需门禁：它仍只在调用方显式提供与目标、编译器和
生成参数匹配的 `compile_commands.json` 时执行。不要为了让它在此 job 中运行而使用
与实际构建不匹配的 compile database；待可重复地生成该数据库后，才能单独启用
`--require-tidy`。

轻量 C++ 测试可逐步包含 `cpp/tests/test_support.hpp`，并用
`MINE_TELEOP_EXPECT` / `MINE_TELEOP_EXPECT_THROWS` 保留现有的异常式 runner，同时在
失败消息中写入调用点文件和行号。该 header 保持 C++17 兼容；其独立验证为：

```bash
scripts/test/check_test_support_header.sh
```

### 2. Ubuntu 22.04 原生构建

```bash
scripts/test/check.sh
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
scripts/build/build_cpp_ubuntu_bundle.sh test linux/amd64
```

`test` 模式生成带 SHA-256 的自包含压缩包，并自动调用
`scripts/test/check_cpp_ubuntu_bundle.sh`。成品门禁校验架构、动态库、GStreamer
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
scripts/build/build_macos_control_bundle.sh test
```

原生架构必须通过编译、CTest、签名、依赖、回环监听、端口冲突、页面脚本、
本地日志脱敏和退出清理。交叉编译的 x64 包只算 build-only，必须在 Intel Mac
或 Rosetta 环境补做运行验收。

双驾驶员、双车辆隔离门：

```bash
scripts/test/check_macos_control_2x2.sh /path/to/cmake-build
```

### 5. 控制面与 TURN

```bash
scripts/test/run_control_plane_docker_smoke.sh
scripts/test/check_coturn_relay.sh
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
