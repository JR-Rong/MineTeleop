# CI 构建与自动发布

工作流：`.github/workflows/ci-release.yml`。

## 触发与交付

- 每次 PR 创建、更新、重开，以及标题或标签修改，执行全部构建并上传测试包；不创建 tag 或 Release。
- 推送到 `main`，以及在 `main` 上手动运行工作流，构建成功后自动发布 `vA.B.C`。
- 其他分支手动运行只生成测试包。
- PR 新提交会取消该 PR 的旧构建。main 构建和发布按同一 concurrency group 串行处理，不中断正在发布的运行；GitHub concurrency 只保留一个 pending run，快速连续推送可能合并为最新一次构建。

| 组件 | 系统/架构 | 验证与产物 |
| --- | --- | --- |
| 控制端 | Windows arm64 / x64 | MSVC 原生编译、进程生命周期测试、包含 Chromium 的 ZIP |
| 控制端 | macOS arm64 | Xcode 26.3、CTest、进程生命周期测试、ad-hoc 签名的独立 App ZIP |
| 控制端 | Ubuntu 22.04 x64 | Docker 原生编译、CTest、进程生命周期测试、运行库闭包检查、桌面 tar.gz |
| 云端 | Ubuntu 22.04 x64 | Docker 编译、CTest、干净容器自检、独立部署 tar.gz |
| 车端 | Ubuntu 22.04 x64 | Docker 全量编译和 CTest；完整部署包还需下述私有运行库 |

所有构建固定到同一源码 SHA。汇总任务逐个校验包内的源码 SHA、版本、桌面架构、干净源码标记以及 SHA-256，生成带版本号的文件名、可移植的 `.sha256` 文件和 `RELEASE-MANIFEST.json`。测试包保留 14 天。

## A.B.C 规则

这是一套项目自定的改动规模规则：

| 改动 | 判定 | 例：从 1.2.3 升级 |
| --- | --- | --- |
| 任意重构 | PR/提交标题以 `refactor:`、`refact:` 或带 scope 的形式开头 | 2.0.0 |
| 大功能 | PR 标记 `release:major`；`feat!:` 等 breaking 标记也升级 major | 2.0.0 |
| 普通功能 | PR/提交标题以 `feat:` 或 `feat(scope):` 开头 | 1.3.0 |
| 修复、CI、打包、测试、文档等维护 | `fix:`、`ci:`、`build:` 等，其余未分类改动默认 patch | 1.2.4 |

标签 `release:major` / `release:minor` / `release:patch` 可以明确最低升级级别；不能把已经识别出的 refactor/feat 降级。一次发布中多个 PR/提交取最高级别。大功能通过标签判断，不依靠 CI 猜测 diff 大小。

从当前提交祖先中版本最高的正式 Release 计算下一个版本。尚无正式 Release 时以 `desktop/package.json` 中的 `0.2.0` 为基础。PR 版本为预估值；main 构建时重新计算，以免其他 PR 先合并后发生冲突。桌面 App 的包版本、云端/车端 BUILD-INFO、归档名和 Release manifest 使用同一个计算结果，不修改源码中的 package.json。

同一已发布提交重跑时复用原版本，保留已发布资产。发布先创建 draft，上传并核对 GitHub 返回的资产大小和 SHA-256 后才公开。上传失败保留 draft，可重跑恢复；不移动已有 tag，不改写已发布资产。只有发布任务有 `contents: write`，PR 构建只读。

## 完整车端包的私有依赖

当前 MineTeleop 为公开仓库，而 ChassisControl 和 MinePilot 为私有仓库。完整车端包必须包含真实 `libchassis_control.so`，并从本次提交重新构建 `libmine_teleop_chassis_bridge.so`。不能把无底盘库的编译测试产物当作完整车端发布包。

**待配置：私有仓库的专用只读访问凭据，以及该二进制是否允许公开发布的明确决定。** 完整打包步骤已经接入，但默认不启用。 在这两项落实前，PR 仍执行车端源码编译/测试；正式发布的全量包检查会因缺少完整车端包而失败，不发布不完整版本。不要将个人 GitHub token 写入工作流、聊天或仓库。

启用方式（仅在允许公开该运行库后）：

1. 分别为 ChassisControl、MinePilot 配置只读 SSH deploy key，把私钥保存为 MineTeleop Actions secrets `CHASSIS_CONTROL_READ_KEY` 和 `MINEPILOT_READ_KEY`。不要使用个人全权限 token。
2. 设置仓库变量 `VEHICLE_RUNTIME_PUBLIC=true`。仅 main/手动运行和本仓库 PR 可以读取私有依赖；fork PR 始终只执行源码编译/测试。
3. 构建输入固定在 `packaging/vehicle/dependencies.lock.json`。初始值沿用本次开发的已知本地底盘头文件/运行库提交，不追随私有仓库的浮动分支。修改底盘实现时应配套更新这两个 SHA 并重新验收。
4. 私有检出目录 `.ci-deps/` 同时排除在 Git 和默认 Docker context 外，头文件和库通过显式构建输入使用；不上传源码或 CI 镜像。

车端现有严格入口是 `scripts/build/build_cpp_ubuntu_bundle.sh test linux/amd64`，内部调用 `prepare_chassis_runtime.sh` 并验证底盘 ABI、动态依赖及部署包。该入口需要：

- `MINE_TELEOP_CHASSIS_CONTROL_ROOT`：包含 `include/global_variables.h` 的 ChassisControl 目录。
- `MINE_TELEOP_CHASSIS_CONTROL_LIBRARY`：MinePilot 提供的 x64 `libchassis_control.so`。

当前编译/打包覆盖不代表 Windows/macOS 的发行证书签名、公证，或目标机器的 GPU、摄像头、CAN 和实车控车验收。

## 本地检查

```sh
python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
actionlint .github/workflows/ci-release.yml
```
