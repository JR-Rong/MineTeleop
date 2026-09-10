# 独立桌面控制端

入口采用 Electron 44.3.0 自带的 Chromium，Windows、macOS、Ubuntu 共用 `desktop/main.cjs`。原有 C++ HTTP 控制服务、网页、WebRTC 和 Three.js 场景继续复用。操作员无需安装 Node、Python、Docker、浏览器或编译工具；这些只用于开发和打包。

## 使用与退出

- Windows 解压完整目录后双击 `MineTeleop.exe`；macOS 打开 `MineTeleop.app`；Ubuntu 解压后打开 `MineTeleop`。不要单独复制入口文件。
- 窗口内填写驾驶员 ID 和密码，然后选择授权车辆、连接和握手。默认包预置三机信令地址、解析规则、CA、网页和模型，不预置驾驶员身份或密码。
- 关闭唯一窗口、菜单“退出控制台”或 macOS Cmd+Q 会退出整个应用。重复启动只聚焦已有窗口。
- 桌面主进程通过私有 stdin 管道拥有原生服务。服务绑定随机的 127.0.0.1 端口，准备就绪后才打开窗口。关闭时先销毁页面，再通知服务清除本地控制输入代次、停止 HTTP 服务并注销；超过 8 秒只终止本应用创建的子进程。主进程异常终止导致管道 EOF 时，原生服务也会退出。
- 本地退出不代表已经收到车辆制动反馈。车端原有输入租约、控制超时和物理急停机制仍独立生效。网络断开时远端注销可能失败，车辆验收需覆盖这种情况。
- 日志保存在操作系统用户数据目录下的 `MineTeleop`：Windows `%APPDATA%`、macOS `~/Library/Application Support`、Linux `~/.config`。不向安装目录写运行日志。

网页无 Node 权限、无 preload/IPC 权限桥，启用 sandbox、contextIsolation、webSecurity；禁止新窗口、任意导航和 webview。应用不调用默认浏览器，不在启动时下载依赖。实现参照 [Electron 安全建议](https://www.electronjs.org/docs/latest/tutorial/security)。

## 网页账号与配置兼容

`POST /api/login` 必须包含 `driver_id` 和 `password`。账号校验限制为非空、最多 128 字节、无空白或控制字符；网页会去除首尾空格。密码提交前清空输入框，不写 YAML、localStorage 或日志。原生服务在登录期间保留密码用于既有信令重启恢复，退出成功后清除缓存。

切换账号必须先退出。退出失败时保留旧 token 供重试，拒绝切换身份。登录、车辆会话切换和退出串行处理，运行中的账号独立于只读配置。旧的 `driver.id` 和显式开发密码参数仍兼容原生开发调用；网页不从配置获取账号，也不使用默认开发密码。

## 构建

先用已有平台脚本生成原生包并解压。开发机使用 Node 22.12+：

```sh
npm ci --prefix desktop
node --test desktop/test/*.test.cjs
node scripts/build/build_desktop_control_bundle.mjs \
  --platform darwin --arch arm64 --native-root /absolute/unpacked/native-bundle
```

Windows 使用 `--platform win32 --arch x64`；Linux 使用 `--platform linux --arch x64` 或 `arm64`。平台和架构必须与原生二进制头一致，打包器会校验。默认配置来自 `configs/driver-console.three-machine.dev.yaml`，需要另一套预置时显式传 `--config /path/driver-console.yaml`，并把对应证书放入原生包 config 目录。

- macOS：`scripts/build/build_macos_control_bundle.sh` 将最低部署版本设为 13.0。桌面包在 macOS 上整体 ad-hoc 签名。面向外部分发仍需 Developer ID 签名与公证；当前测试包没有这些发布凭据。修改签名包内的模型后，应重新打包和签名。
- Windows：`scripts/build/build_windows_control_bundle.ps1` 使用已有锁定 MSVC/vcpkg 构建；CMake `InstallRequiredSystemLibraries` 将编译器运行库复制到 EXE 旁，打包时检查 `vcruntime140.dll` 和 `msvcp140.dll`。CI 增加 Node 桌面测试和 Chromium 桌面 ZIP，原生诊断包继续保留。见 [CMake 运行库分发说明](https://cmake.org/cmake/help/latest/module/InstallRequiredSystemLibraries.html)。
- Linux：最终打包须在目标架构 Ubuntu 22.04 构建环境运行；构建机需 `python3`、`patchelf`、`ldd`、GTK3/NSS/NSPR/ALSA/GBM 等开发环境运行依赖。`collect_linux_desktop_runtime.py` 收集非系统动态库，设置可重定位 RPATH 并检查解析结果。仅保留 glibc/加载器及 GPU 驱动接口由桌面 OS 提供，不采用需要 FUSE 的 AppImage。打包器不禁用 Chromium sandbox。

输出在 `dist/desktop`：完整应用目录、ZIP（Linux 为 tar.gz）、SHA-256、`DESKTOP-BUILD.json` 文件清单。容器中无 Git 时，传入已核对的 `--source-commit <40位SHA> --source-dirty true` 记录源码快照，不能把工作区构建冒充干净提交。

开发调试（Electron npm 包首次可能需要显式安装运行时）：

```sh
node desktop/node_modules/electron/install.js
MINE_TELEOP_DESKTOP_ROOT=/absolute/unpacked/native-bundle \
  node desktop/node_modules/electron/cli.js "$PWD/desktop"
MINE_TELEOP_TEST_NATIVE_ROOT=/absolute/unpacked/native-bundle \
  node --test desktop/test/*.test.cjs
```

## 本次验证与限制

- macOS 原生 CTest 8/8；新增双账号登录/权限隔离测试通过。真实原生服务的退出指令、父管道 EOF、SIGTERM 三种退出路径均验证端口关闭；桌面测试合计 9/9。
- macOS arm64 桌面包实际打开，登录框、自车模型正常显示；开发窗口与打包窗口点击关闭后主进程、原生服务、Chromium 子进程和监听端口均退出。打包版签名递归校验通过，原生程序 Mach-O 最低版本为 13.0。
- Ubuntu 22.04 arm64 原生账号回归通过，打包后 113 个 ELF 依赖解析检查通过，包内服务生命周期测试 9/9；Windows x64 原生代码 MinGW 交叉编译通过。Windows 当前本地产物是交叉编译测试包，MSVC 的 CI 新步骤尚未在线执行，不能把它当作已验收的正式 Windows 发布包。
- Windows/Ubuntu 干净桌面系统、macOS 13 最低版本、真实摄像头解码、硬件输入和车辆链路仍需目标机验收。自带内核统一浏览器实现，但不同 GPU 的 WebGL/WebRTC 硬件能力仍可能不同，尤其 H.265；需验证既有 H.264 回退。
- 环视拼接、相机角色绑定和障碍物接入继续暂缓。

本次测试包体积（含默认矿车模型）：

| 平台 | 压缩包 | 解压文件合计 |
| --- | ---: | ---: |
| Windows x64（MinGW 测试包） | 172.0 MiB | 418.6 MiB |
| macOS arm64 | 134.6 MiB | 321.0 MiB |
| Ubuntu 22.04 arm64 | 168.9 MiB | 398.6 MiB |

压缩包位于 `dist/desktop`。以上不包含用户运行后的缓存和日志，后续 MSVC Windows 正式构建大小可能不同。记录与签名校验通过，工作区未提交，因此 manifest 标注 `source_dirty: true`。
