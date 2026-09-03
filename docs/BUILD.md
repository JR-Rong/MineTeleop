# Mine Teleop 编译教程

本文只描述如何从源码生成三端安装包。安装、凭据配置和启动步骤见
[DEPLOY.md](DEPLOY.md)。

## 1. 产物与构建入口

| 端 | 目标平台 | 构建脚本 | 产物 |
| --- | --- | --- | --- |
| 云端 | Ubuntu 22.04 amd64 | `scripts/build/build_macos_cloud_bundle.sh` | `dist/mine-teleop-cloud-ubuntu22.04-x64-*.tar.gz` |
| 控制端 | macOS arm64/x64 | `scripts/build/build_macos_control_bundle.sh` | `dist/mine-teleop-control-macos-*.tar.gz` |
| 控制端 | Windows x64/arm64 | `scripts/build/build_windows_control_bundle.ps1` | `dist/mine-teleop-control-windows-*.zip` |
| 车端 | Ubuntu 22.04 amd64 | `scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64` | `dist/mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz` |

所有构建脚本都会同时生成 `.sha256` 文件。安装包不包含司机密码、车辆
device token 或 TURN shared secret。

## 2. Mac 构建机准备

支持 Intel Mac 和 Apple Silicon Mac。需要：

- Git；
- Docker Desktop 或 Colima；
- `docker buildx`；
- 构建控制端时需要 Xcode Command Line Tools。

检查：

```bash
git --version
docker info
docker buildx version
xcrun --find clang++
```

以下命令均从仓库根目录运行：

```bash
cd /path/to/mine-teleop
```

所有编译脚本默认只编译和打包，不编译测试用例，也不运行测试。只有显式传入
`test` 参数时才编译并运行测试；`test` 可以放在其它位置参数之前或之后。

## 3. 构建云端安装包

```bash
scripts/build/build_macos_cloud_bundle.sh
```

该脚本只构建信令服务及云端部署资源，不构建 GStreamer、相机桥接或车端
runtime。需要同时编译测试并在 Ubuntu 22.04 amd64 容器中运行包级自检时：

```bash
scripts/build/build_macos_cloud_bundle.sh test
```

自定义输出目录：

```bash
scripts/build/build_macos_cloud_bundle.sh \
  linux/amd64 \
  "$PWD/dist/cloud-release"
```

## 4. 构建 macOS 控制端

Apple Silicon：

```bash
scripts/build/build_macos_control_bundle.sh
```

同时编译并运行控制端测试和最终压缩包验收：

```bash
scripts/build/build_macos_control_bundle.sh test
```

Intel Mac：

```bash
MINE_TELEOP_MACOS_ARCH=x64 scripts/build/build_macos_control_bundle.sh
```

交叉构建且当前 Mac 无法执行目标架构程序时，直接使用默认构建即可生成
build-only 包；不要传入 `test`：

```bash
MINE_TELEOP_MACOS_ARCH=x64 \
  scripts/build/build_macos_control_bundle.sh
```

build-only 包必须在同架构 Mac 上补做运行验收后才能交付。

## 5. 构建 Windows 控制端

Windows 构建机需要：

- Windows 10/11；
- Visual Studio 2022，并安装“使用 C++ 的桌面开发”；
- CMake；
- vcpkg，且已设置 `VCPKG_ROOT`。

在 PowerShell 中从仓库根目录运行：

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
powershell -ExecutionPolicy Bypass -File .\scripts\build\build_windows_control_bundle.ps1
```

脚本默认使用 `x64-windows`，自动安装控制端依赖和收集 app-local DLL，只构建
`mine-teleop-control`，并生成 ZIP 与 SHA-256。若依赖已准备好，可跳过安装：

```powershell
.\scripts\build\build_windows_control_bundle.ps1 -SkipDependencyInstall
```

生成包前额外运行 `mine-teleop-control.exe --help` 启动检查：

```powershell
.\scripts\build\build_windows_control_bundle.ps1 -SmokeTest
```

构建 arm64 包：

```powershell
.\scripts\build\build_windows_control_bundle.ps1 -Architecture arm64
```

当前仓库尚未记录 Windows 实机编译和运行验收证据，因此生成的 ZIP 必须在目标
Windows 上完成启动、仅回环监听、浏览器、端口冲突、HTTPS/WSS 证书、登录、媒体、
DataChannel、急停、安全释放和退出后端口清理检查，不能仅凭生成压缩包视为交付通过。

## 6. Apple Silicon Mac 从零构建车端

推荐沿用统一入口：

```bash
scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

在 Apple Silicon Mac 上且未设置 `MINE_TELEOP_BASE_BUNDLE_ARCHIVE` 时，该入口
会自动调用：

```bash
scripts/build/build_macos_vehicle_from_scratch.sh
```

这个路径不依赖历史基础包。它会：

1. 从干净的 Ubuntu 22.04 amd64 镜像准备目标 sysroot；
2. 在 Apple Silicon 上原生运行 amd64 交叉编译器；
3. 构建固定提交和 SHA-256 的 OpenSSL libsrtp；
4. 构建当前 Mine Teleop C++ 目标；
5. 收集动态库、GStreamer 插件、CA 和相机辅助文件。

显式传入 `test` 时，才会额外编译测试用例、在 amd64 容器中运行 libsrtp
CTest 和 Mine Teleop CTest，并对最终 `.tar.gz` 运行干净 Ubuntu 容器验收：

```bash
scripts/build/build_cpp_ubuntu_bundle.sh test linux/amd64
```

默认单线程编译以减少内存和工具链抖动。需要调整时：

```bash
MINE_TELEOP_BUILD_JOBS=2 \
  scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

使用其他车端 YAML：

```bash
MINE_TELEOP_VEHICLE_CONFIG="$PWD/configs/my-vehicle.yaml" \
  scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

### 底盘 bridge 运行库

车端安装包强制包含
`lib/vendor/chassis/libmine_teleop_chassis_bridge.so` 和
`lib/vendor/chassis/libchassis_control.so`。这两个构建/授权物料不提交到 Git。
统一打包入口每次都执行以下流程，从当前 checkout 重建 bridge；不会复用
`vendor/chassis/lib` 中残留的旧 bridge：

```bash
MINE_TELEOP_CHASSIS_CONTROL_ROOT=/path/to/ChassisControl \
MINE_TELEOP_CHASSIS_CONTROL_LIBRARY=/path/to/libchassis_control.so \
  scripts/build/prepare_chassis_runtime.sh
```

未设置变量时，默认读取相邻目录 `../ChassisControl` 和
`../MinePilot/libchassis_control.so`。Docker bundle 阶段和最终 tar.gz 检查都会
验证文件存在且 bridge 的动态依赖全部可解析；因此缺少底盘库时构建会明确失败，
不会再生成表面通过、运行时才报 `dlopen` 失败的安装包。

## 7. 原生 Ubuntu amd64 构建车端

在 Ubuntu 22.04 amd64 构建机上仍使用统一入口：

```bash
scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

如果只有应用源码变化，并且已有经过验收且带校验和的基础包，可以复用其中
体积较大的 codec/GStreamer 运行库：

```bash
MINE_TELEOP_BASE_BUNDLE_ARCHIVE=/path/to/accepted-vehicle-bundle.tar.gz \
  scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

该模式仍会重新构建当前 Mine Teleop 二进制和 Chassis bridge，并从当前固定
源码刷新整组 Aravis 运行库，避免新 camera binary 与旧同 SONAME 库发生符号
不匹配；因此同样需要 ChassisControl headers 与运行库输入。需要重新验证最终
安装包时追加 `test` 参数。

## 8. 构建后检查

查看产物和校验和：

```bash
ls -lh dist/*.tar.gz dist/*.tar.gz.sha256
vehicle_archive="$(ls -t dist/mine-teleop-vehicle-ubuntu22.04-x64-*.tar.gz | head -n 1)"
shasum -a 256 "$vehicle_archive"
```

独立复验车端包：

```bash
scripts/test/check_cpp_ubuntu_bundle.sh "$vehicle_archive"
```

该检查会在干净的 Ubuntu 22.04 amd64 容器内同时加载随包的 chassis bridge，
运行 `config-check --chassis-bridge-library ...`，并明确要求输出
`chassis_bridge_abi.version=4`。ABI 版本、V4 配置结构大小或
`mine_teleop_chassis_open_v4` 任一不匹配都会使安装包复验失败；runtime 与 bridge
必须成套发布。

独立复验控制端包：

```bash
control_archive="$(ls -t dist/mine-teleop-control-macos-*.tar.gz | head -n 1)"
scripts/test/check_macos_control_bundle.sh "$control_archive"
```

完整测试矩阵和硬件验收边界见
[11-testing-and-validation.md](11-testing-and-validation.md)。

## 9. 常见构建问题

### Docker 未运行

```bash
docker info
```

先启动 Docker Desktop 或 Colima，再重新执行构建脚本。

### APT 下载失败

脚本已配置重试和超时。先确认 Docker 容器可以访问 Ubuntu 软件源，再重新
运行；不要在工控机上改为临时手工编译。

### 磁盘空间不足

首次车端构建会缓存 Ubuntu amd64 依赖和交叉编译层。检查：

```bash
docker system df
df -h
```

只清理明确无用的镜像和缓存，不要删除正在使用的安装包或运行容器。
