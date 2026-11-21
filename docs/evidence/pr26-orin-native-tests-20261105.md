# PR #26：Orin 台架原生测试记录

日期：2026-11-05 晚（UTC 2026-11-06 07:38–08:09）。被测提交 `f1ae44d`。
范围：把容器里构建的 aarch64 测试二进制放到 Orin 上，在无特权的私有命名空间里跑 PR 自带的原生测试。

## 结论（先说结果）

- 85 个注册测试一遍：**76 通过、5 跳过、4 失败**。9 个未通过项各有环境原因（下表），没有一项指向被测代码。
  其中 `chassis-bridge-diagnostics-smoke` 在给它一个私有 `/var/log` 后补跑通过；其余 8 项在 Orin 上没有跑通，原因是缺少测试依赖，没有在补齐依赖的 Orin 上重跑。
- 84 次重复运行**零失败**，`[RETRY]` 出现 **0** 次：保持夹具 20、product-direct 30、control-transport 10、media-time-sync 20、control-tests 4。
- `media-time-sync-retry-loopback` 在 Colima 虚拟机里约三分之一失败（阶段 2 基线二进制上 4/12），在 Orin 上 20/20 通过。
  如果失败率和 Colima 相同，连续 20 次通过的概率约 0.03%。这**支持**"偶发来自虚拟机的挂钟校正"这一解释，但没有改变过任何环境条件，不是干预验证。
- 这些测试以定时器和睡眠为主，Orin 上的耗时与 Colima 基本相同（差异在 7% 以内），所以这次运行**没有**给出 CPU 余量的信息，也不是负载测试。

## 环境

| 项 | 值 |
| --- | --- |
| 机器 | `nvidia@192.168.2.2`（Jetson Orin 系列，6 核，7.6 GB 内存），Ubuntu 22.04.5，L4T R36.5，内核 5.15.185-tegra，aarch64 |
| 软件 | GStreamer 1.20.3，ctest 3.22.1，Python 3.10.12；**没有** `avdec_h264`（gst-libav）、`turnserver`、node；内核未编译 `NET_SCH_NETEM` |
| 二进制 | Ubuntu 22.04 容器（aarch64，GStreamer 1.20.3，libnice 0.1.18）里构建；与 `f1ae44d` 的 482 个受跟踪文件逐个核对一致，对构建树做 `make -n` 干跑，没有待编译或待链接的项 |
| 唯一缺的库 | `libyaml-cpp.so.0.7`（73 个文件共需 71 个共享库，其余 Orin 都有），随包放在目录内并用 `LD_LIBRARY_PATH` 指向 |
| 包 | 201 MB tarball，SHA-256 `ad8622bef85d9b0b3ad133053580b913017388a2ff34e2da9ea559d447765a1b`；解压后 719 MB、563 个文件逐个校验通过 |

## 隔离方式

Orin 上有真实的 `can0`（状态 DOWN）以及 `gbe0`、`gbe1`、`l4tbr0`、`usb0/1`、`docker0`。因此所有测试都在
`unshare --user --map-root-user --net --mount --pid --fork --mount-proc` 创建的命名空间里运行：

- 套接字只看得到 `lo`；每次进入前先检查 `socket.if_nameindex()` 是否恰好是 `lo`，否则退出。
- `/tmp` 是私有 tmpfs；`HOME`、GStreamer 注册表和缓存都指向包目录，没有碰用户的 `~/.cache`。
- PID 命名空间生效（命名空间内 shell 的 PID 为 1，可见进程 4 个），退出时子进程全部消失；结束后又按目录路径清扫，结果为 0 个残留。
- 环境变量用 `env -i` 清空后只放必要的几项，`nice -n 10` 运行，单进程顺序执行。
- 旁证：NVIDIA 的 Argus 相机插件在加载时尝试连接 `nvargus-daemon`，因为私有 `/tmp` 里没有它的套接字而失败，也就是测试连相机守护进程都没有碰到。

## 全量一遍（`ctest -E <9 项>`，212.66 s）

排除的 9 项：需要 node 的 7 项（`native-web-control-parity`、`camera-view-tests`、`session-logs-js-tests`、`scene-state-tests`、
`control-logic-js-tests`、`control-transport-js-tests`、`ice-page-tests`）和需要编译器与开发头文件的 2 项（`ice-vehicle-callback-mock-tests`、`ice-gstreamer-init-tests`）。

| 结果 | 数量 |
| --- | --- |
| 通过 | 76 |
| 跳过 | 5 |
| 失败 | 4 |

### 未通过的 9 项

| 测试 | 结果 | 原因 | 依据 |
| --- | --- | --- | --- |
| `native-receiver-mid-integration-tests`、`native-receiver-swapped-mid-tests`、`native-receiver-software-frames-tests` | 跳过（返回 77） | 缺 `avdec_h264` | 源码在工厂清单里缺任何一个元素就返回 77，`gst-inspect-1.0 avdec_h264` 在 Orin 上找不到（`openh264dec`、`nvv4l2decoder` 有） |
| `codec-roundtrip-test` | 失败 | 缺 `avdec_h264` | 输出 `decoder factory unavailable`，阶段为 `factory_inventory`，`x264enc` 可用 |
| `recording-gstreamer-tests` | 失败 | 缺 `avdec_h264` | `parse no element "avdec_h264"` |
| `media-freshness-gstreamer-tests` | 失败 | 缺 `avdec_h264` | 前半段 `[PASS] bounded IDR retry, timeout, old-identity barrier and recovery metrics`，后半段 `offline pipeline parse failed: no element "avdec_h264"` |
| `ice-fallback-gstreamer-tests` | 跳过 | 缺 `turnserver` | 脚本 `shutil.which('turnserver')` 为空即返回 77 |
| `transient-hold-netem-tests` | 跳过 | 内核没有 netem | `/proc/config.gz`：`# CONFIG_NET_SCH_NETEM is not set`；测试里 `tc` 失败即返回 77。**真实回环丢包只在 Colima 里验证过** |
| `chassis-bridge-diagnostics-smoke` | 失败，补跑通过 | 该用例用默认 VCU 日志路径 `/var/log/mine-teleop/vcu-can.jsonl`，无特权命名空间不能在 `/var/log` 下建目录 | 日志里 `vcu_log_directory_create_failed ... Permission denied`；给命名空间一个私有的 `/var/log` tmpfs 后 48.31 s 通过 |

关于最后一项：默认路径 `/var/log/mine-teleop/vcu-can.jsonl`（环境变量 `MINE_TELEOP_VCU_LOG_PATH` 可改）在 PR25 基线 `05bb17d` 之前就存在，不是本 PR 引入。
这个用例没有为该场景设置 `MINE_TELEOP_VCU_LOG_PATH`，所以以 root 运行测试套件会在真实 `/var/log` 下创建目录，非 root 运行则失败。

## 重复统计（每次单独调用 `ctest -R <名称> -V`）

| 测试 | 次数 | 通过 | 失败 | 单次耗时（通过的，秒） | `[RETRY]` | Colima 对照 |
| --- | --- | --- | --- | --- | --- | --- |
| `transient-hold-integration-tests` | 20 | 20 | 0 | 11.56–11.81 | 0 | 容器内 11.4 |
| `product-direct-integration-tests` | 30 | 30 | 0 | 10.54–10.79 | 0（`[RETRY-SUMMARY] total=0` 30 次） | 本地累计 70 次零重试 |
| `control-transport-integration-tests` | 10 | 10 | 0 | 10.57–11.07 | 0 | 容器内 10.4 |
| `media-time-sync-retry-loopback` | 20 | 20 | 0 | 13.33–13.39 | — | **约 1/3 失败** |
| `control-tests` | 4（另有全量里 1 次） | 4 | 0 | 54.75–55.47 | — | 容器内 54.7 |

如果 `product-direct` 的真实失败率是 10%，30 次全过的概率约 4%；这只说明失败率大概率低于一成，不是稳定性证明。
`control-tests` 里有 50 ms 余量的登录锁定恢复用例，只重复了 5 次，证据很弱。

## 配置检查（`vehicle-agent --preflight`，真实二进制）

- 带保持的台架配置（`control_timeout_ms: 1200`、`bridge_apply_timeout_ms: 800`、`transient_hold` 块，其余同 `vehicle-agent.orin-cpu-bench.yaml`）：`ready: true`，生效值里有保持设置。
- 三种不安全配置都被拒绝（退出码 2）：
  `control.transient_hold requires degraded_timeout_ms + max_hold_ms + 100 <= control_timeout_ms`、
  `control.bridge_apply_timeout_ms must be 20..60000 and no greater than control_timeout_ms`、
  `control.transient_hold.bogus_setting is not a known setting`。
- `vehicle-agent --run-loop` 不能用来验证保持：它从不提交会话控制 profile，所以任何配置下都停在 STANDBY（该门槛由 `320a662` 引入，早于本 PR）。

## 这次能说明什么，不能说明什么

能说明：
- 同一份代码编出的 aarch64 二进制在 Orin 真机和 5.15-tegra 内核上，85 项里 76 项通过，其余 9 项因环境缺失未通过（其中 1 项补跑通过）；瞬态保持的真实定时器夹具 20/20 通过。
- 夹具重试在这台机器上 84 次运行里一次也没有出现。
- `media-time-sync` 在这台没有虚拟机时钟校正的机器上，20 次都没有出现 Colima 里的偶发失败。

不能说明：
- 真实 WebRTC 双端会话、5G、TURN、真相机与编码硬件、真实 CAN 与车辆。驱动端对端是浏览器或 Windows 查看器，这里没有可运行的。
- 真实网络丢包与尖峰：Orin 内核没有 netem，该用例在 Orin 上没有执行。
- 缺 `avdec_h264` 导致 6 个测试没有在 Orin 上跑通（3 个跳过、3 个失败），它们在容器里通过。车端运行库不引用 `avdec_h264`（只有测试、探针和台架代码引用它），所以这不影响车端产品行为，但这几项没有 Orin 上的结果。
- CPU 余量或负载下的行为：测试是定时器主导的，且没有真实相机与编码负载。

## 处理与恢复

- 运行结束后清扫（0 个残留进程），按用户要求删除了 `~/mine-teleop-tests/pr26-transient-hold-20261105`（719 MB、656 个文件）。
- 前后快照对比：`~/mine-teleop-tests` 其余内容、用户家目录顶层、`~/.cache/gstreamer-1.0/registry.aarch64.bin` 都没有变化；
  变化只有 `~/mine-teleop-tests` 目录自身的修改时间（创建又删除子目录），以及宿主 `/tmp` 多了一个 root 的 `systemd-private-…-fwupd.service-…`（systemd 在 07:57:46Z 为固件更新服务创建，与本次运行无关）。

## 复现

脚本在运行时放在仓库之外（`/tmp/mt-scratch/orin/`）：`build_bundle.sh`（打包）、`rehearse.sh`（容器内演练）、`isolated.sh`（命名空间）、
`experiments.sh` 与 `run_all.sh`（阶段与顺序）、`sweep.sh`（按目录路径清扫）、`watch.sh`、`analyze_results.py`。临时目录可能已被清理，目前没有入库。
原始日志取回在 Mac 的 `/tmp/mt-scratch/orin-results-20261106-run1/`（336 KB，含补跑日志）。
