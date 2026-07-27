# Mine Teleop 编译教程

本文只描述如何从源码生成三端安装包。安装、凭据配置和启动步骤见
[DEPLOY.md](DEPLOY.md)。

## 1. 产物与构建入口

| 端 | 目标平台 | 构建脚本 | 产物 |
| --- | --- | --- | --- |
| 云端 | Ubuntu 22.04 amd64 | `scripts/build/build_macos_cloud_bundle.sh` | `dist/mine-teleop-cloud-ubuntu22.04-x64-*.tar.gz` |
| 控制端 | macOS arm64/x64 | `scripts/build/build_macos_control_bundle.sh` | `dist/mine-teleop-control-macos-*.tar.gz` |
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

## 5. Apple Silicon Mac 从零构建车端

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

## 6. 原生 Ubuntu amd64 构建车端

在 Ubuntu 22.04 amd64 构建机上仍使用统一入口：

```bash
scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

如果只有应用源码变化，并且已有经过验收且带校验和的基础包，可以复用其中
第三方媒体运行库：

```bash
MINE_TELEOP_BASE_BUNDLE_ARCHIVE=/path/to/accepted-vehicle-bundle.tar.gz \
  scripts/build/build_cpp_ubuntu_bundle.sh linux/amd64
```

该模式仍会重新构建当前 Mine Teleop 二进制。需要重新验证最终安装包时追加
`test` 参数。

## 7. 构建后检查

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

独立复验控制端包：

```bash
control_archive="$(ls -t dist/mine-teleop-control-macos-*.tar.gz | head -n 1)"
scripts/test/check_macos_control_bundle.sh "$control_archive"
```

完整测试矩阵和硬件验收边界见
[11-testing-and-validation.md](11-testing-and-validation.md)。

## 8. 常见构建问题

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
