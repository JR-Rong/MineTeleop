# PR #26 说明重写稿（草稿，待审阅）

> 这是替换现有 PR 说明的草稿，**尚未写入 PR**。现有说明约 9,200 字，按时间堆叠了各轮验收记录；
> 这份稿按"当前状态 → 生产改动 → 台架工具 → 已删除 → 升级回退 → 限制与验证"重排。
> 旧说明里的历史实测数字不在这里重复，原始记录在 `docs/evidence/`（迁出方案见 Task 28）。
> 标 `【…】` 的位置需要你填写或决定；其余数字能在文中引用的提交、文件和 `docs/evidence/` 记录里复核。

---

## 当前状态

本 PR 基于 PR25（`rjr/cx/orin-isx031-cpu`），目标分支不变，不向主分支合入。
本轮按审查意见修复生产路径，删除未通过验收的 mock 双路实验（DC/WSS 镜像、紧凑控制与遥测格式），
并实现瞬态链路保持（设计见 [docs/64](../../64-transient-link-hold-design.md)）。
Windows 与 Orin 的 30 分钟 mock 控制验收此前多轮未通过，本 PR 不声称通过，也不声称真实车辆安全。
下文的验证覆盖 Linux 容器、Orin 台架上的原生测试和 CI；真实双端会话尚未验证。

## 生产改动

只列会改变车端或驾驶端产品行为的内容，每条给出提交，便于审查。

1. **ICE 服务与回退**
   - 车端 ICE 服务列表来自认证信令。车端 YAML 里的 `ice.stun_servers` / `ice.turn_servers` 从未被读取，现在出现即拒绝启动，
     错误为 `vehicle ice.stun_servers/turn_servers are unsupported`（升级步骤见下，`855d200`）。
   - `cloud.ice_direct_timeout_ms` 默认仍为 0。启用后，直连超时才回退一次 TURN，每次 runtime 最多一次，
     迟到的 answer、candidate、promise、DataChannel 受代际围栏约束。
   - 回退或重协商时刷新 TURN 凭据失败，改为有界重试并沿用上一份列表，不再让媒体运行时或浏览器协商中止；
     停止请求仍然传播，仅中转的尝试在无法刷新时仍然失败（`d250abd`）。
   - libnice 选中对观测改为尽力而为：读不到时发诊断并继续，不再因为读不到而放弃所有编码器候选、导致没有视频（`513005b`）。
   - 候选和选中对的诊断只记录类型、协议、地址族等脱敏信息，不输出 IP 或接口名。
2. **视频新鲜度与 IDR 补发**：发送端 freshness probe 拒绝旧的或重复的 IDR 与 CORRUPTED AU，
   每次故障最多补发一次 IDR，只有新鲜、完整、身份更新的 IDR 才恢复发送资格，且不能单独恢复控制授权。
   既有的 800 ms / 5 s 保护和控制围栏没有放宽。
3. **receive-only 模式**：原生接收端会话与控制隔离，接收探针使用 receive-only 运行时（`48cfa34`、`2f432d1`）。
4. **DataChannel 试探预算**：直连试探因会话控制 profile 未就绪被拒后，下一次经过认证的、新鲜的中性 WSS 接受恰好授予一个新的有界预算（每个 attempt 一次），
   不绕过退避、代际围栏和 DC 健康检查（`f0a8e33`）。该行为由 `confirmed_profile_permits_only_one_new_bounded_dc_budget` 覆盖。
5. **瞬态链路保持与硬看门狗**（`transient-hold-v1`，设计见 [docs/64](../../64-transient-link-hold-design.md)）
   - 控制链路延迟或迟到命令、驾驶端或车端视频短暂失鲜时，先进入"保持"（上限 700 ms）：牵引恒为 0，转向和挡位保持，制动取驾驶员制动与 0.15 轻制动的较大值；
     连续 2 条新鲜命令后自动恢复。超过上限、预算用尽（60 s 内 6 次、累计 3000 ms）或出现故障类原因，立即进入原有的制动等待。
   - 急停、输入租约、校时、控制链路未就绪、视频或相机故障、非法样本、故障类制动修订号、失去控制权、底盘/CAN 故障的行为不变；制动在一次停车中只增不减。
   - 现场配置 `vehicle-agent.three-machine.field.yaml` 和 `vehicle-agent.ccg2-8m.yaml` 显式开启，
     `control_timeout_ms` 由 800 改为 1200（硬看门狗），新增 `bridge_apply_timeout_ms: 800`（底盘桥自己的 apply 看门狗，仍为 800）。
     YAML 没有 `transient_hold` 块时视为关闭，开发、台架和 Orin 示例配置不变。
   - 启动校验：`degraded_timeout_ms + max_hold_ms + 100 ≤ control_timeout_ms`，字段越界或拼错键名拒绝启动。
   - 协商：车端在 offer 里声明能力，驾驶端确认后才在签名命令里携带 `transient_hold` / `critical_video_transient` 字段；
     云端代码不改，Windows 原生 mock 客户端不参与协商。
   - 驾驶端把只是 ACK 往返偏高的情况从"立即停车"改为"持续达到容忍上限才停车"；页面把视频异常分为瞬态与故障上报，保持期间不清除输入。
6. **隔离 mock 的车端证明**：车端只在自己的配置是 `test_bench_` 身份、mock 适配器、零速（`max_speed_kph: 0`）、`commissioning_mode: bench` 时，才在 offer 里声明
   `isolated-mock-bench-v1`；Windows 查看器没有这个证明就拒绝启用 mock 控制（`9e560ed`）。
   这是对"本地 YAML 无法证明远端车辆是隔离台架"的修正，不影响真车。

## 台架工具

均不属于运行库，正式包默认不包含台架诊断。

- Windows 原生 mock 查看器：`cpp/apps/windows_native_viewer.cpp`、`packaging/windows/run-native-viewer.ps1`，由 `.github/workflows/windows-native-receiver-bench.yml` 编译。
- 原生接收端探针 `mine-teleop-native-receiver-probe`：只接收视频，拒绝 `control_enabled: true` 的配置。
- Electron 台架 `desktop/bench/`（含 TURN 白名单与网络策略测试）。
- 离线分析与探针：`scripts/test/codec_roundtrip_probe.py`（opt-in 编解码往返探针，没有接入车辆启动选择）、
  `receiver_ab_analyze.py`、`hybrid_receiver_analyze.py`、`native_mock_input_analyze.py`、`ipv6_dtls_metadata_capture.py`、
  `transient_hold_analyze.py`（保持/恢复/升级的次数与时长分布）。
- 台架网络脚本：`scripts/test/bench_signaling_keepalive.test.py` 与 `configs/bench-signaling-keepalive.Caddyfile`（信令 keep-alive 台架），
  `scripts/test/cellular_interface_failover.test.sh`（需要 root 和网络命名空间，没有登记进 CTest）。
- 台架专用头文件在 `cpp/bench/include/mine_teleop/bench/`，`scripts/test/check_bench_header_links.py` 约束运行库不得依赖它们。
- CMake 选项 `MINE_TELEOP_ENABLE_BENCH_EXPERIMENTS`：本地默认 ON，正式包的 Dockerfile 与脚本默认 OFF；
  隔离 mock 的逐命令追踪只在该选项开启、显式请求且车端有证明时才工作（见 [docs/BUILD.md](../../BUILD.md) 的"台架包与正式包"）。
- 仍保留的 mock 专用逻辑：`mock_idle_direct_reprobe`（默认关闭，由 Windows 查看器的 mock 控制模式显式打开）。

## 已删除的实验

- DC/WSS 镜像：同一认证序号的 DC/WSS 副本、车端的 WSS 到 DC 准入例外、DC 结果与状态背压、镜像接收计数、DataChannel 上的媒体状态副本（`c5f5952`、`cd87603`）。
- mock 专用紧凑 CBOR 控制与遥测格式（`1299127`）。这两个实验没有产出可信证据。
- 依赖它们做比较的 4 份证据记录已标注"已失效"，没有删除（`9b5139c`）。

## 升级与回退

- **车端 YAML**：删除 `ice.stun_servers` / `ice.turn_servers`。自维护配置先运行 `./bin/mine-teleop-run config-check --config <配置>`，
  看到上述错误即按此处理。STUN/TURN 改由信令服务 `--stun-urls` / `--turn-urls` 配置。使用仓库内打包配置的车辆不受影响。
- **瞬态链路保持（可选）**：自维护配置在 `control:` 下加入 `transient_hold:` 块（`enabled: true`，其余取默认值），
  并设 `control_timeout_ms: 1200`、`bridge_apply_timeout_ms: 800`。云端不需要更新；驾驶端与车端同提交更新。
- **回退保持**：`transient_hold.enabled: false`，`control_timeout_ms: 800`，去掉 `bridge_apply_timeout_ms` 或设为不大于 800，重启车端服务，不需要重新发包。
- **验收保持**：导出车端 `vehicle-runtime.log`，运行 `python3 scripts/test/transient_hold_analyze.py <日志文件...>`。
- **正式包与台架包**：需要台架诊断的包，构建时设 `MINE_TELEOP_ENABLE_BENCH_EXPERIMENTS=ON`；默认的正式包构建不带。
- 本 PR 不合入主分支，不改变已推送提交；合并时 squash 还是保留历史由你决定。

## 已知限制与验证情况

**CI**：`【用户填写：推送后的检查链接】`。
- Windows x64 receive-only probe 在 `588acd7`、`f31a932`、`014cbe2` 上都通过：MSVC Release 编译了 `mine-teleop-windows-viewer`（阶段 2、3、4 改动过的 `windows_native_viewer.cpp`），查看器观察测试、containment 测试、DataChannel 与相机 MID 回环测试和 `-CheckOnly` 都通过。下载约 494 MiB 的 GStreamer SDK 被对方服务器断开是常态（三次运行分别断开 1、3、1 次），本轮加了重试和断点续传；最近一次在 513.8 MB 处被断开，续传后完成，SHA-256 校验通过。
- macOS 上的 `transient-hold-integration-tests` 在 CI 上失败过三次，原因已查明：夹具每轮只读一条积压事件，读取速度追不上 20 Hz 的命令流，保持用满 700 ms 后升级；不是产品行为问题。夹具已改为读空积压，在容器里用每轮加 15 ms 的方式复现并确认修复（旧夹具 0/6，新夹具 6/6）。**修复后在 GitHub 的 macOS 运行器上（`014cbe2`）：3 次有效运行 3 次通过（`transient-hold-integration-tests` 用时 11.4–11.8 秒），另有 1 次重跑因 GitHub 没有分到 macOS 运行器被取消，没有结果。之前三个提交上是 3 次失败。样本很少，不能说已经证明稳定。**

**已做的本地验证**（命令与结果，未做的在下一节单列）

| 环境 | 内容 | 结果 |
| --- | --- | --- |
| Linux 容器（Ubuntu 22.04 aarch64，GStreamer 1.20.3，libnice 0.1.18，无外部网络） | 全量 CTest，台架诊断开关 ON 与 OFF 各一次 | 94/94、94/94（含容器内真实 netem 丢包用例） |
| 同上 | `media-time-sync-retry-loopback` 单独重复 | 约三分之一失败；在阶段 2 结束的基线二进制上同样失败（4/12），与负载无关。完整 CTest 因此约三分之一会出现这一项失败；上一行的 94/94 是它通过的那几次运行 |
| Orin 台架（Jetson Orin 系列，6 核，L4T R36.5；无特权私有命名空间，只有回环；2026-11-05 晚） | 全量 85 项（排除需 node 的 7 项和需编译器的 2 项），另重复 84 次 | 76 通过、5 跳过、4 失败；9 项未通过均因环境（缺 `avdec_h264`、缺 turnserver、内核没有 netem、`/var/log` 无写权限），其中 1 项补跑通过；84 次重复零失败，`[RETRY]` 0 次；`media-time-sync` 20/20 通过（Colima 约 1/3 失败，只是旁证，不是干预验证） |

Orin 记录：`docs/evidence/pr26-orin-native-tests-20261105.md`。

**偶发与未查明的现象**
- `media-time-sync-retry-loopback`（`cli-over-budget`）：比较同一次交换的挂钟与单调钟耗时。运行容器的虚拟机会周期性校正客户机挂钟（日志里相邻两次约 30 秒），实测会出现约 100 ms 的回拨，与现象吻合，但没有对这一点做干预验证。
- 集成夹具里有三处"夹具重试"。已改为每次重试都带原因打印并计数（`[RETRY]`、`[RETRY-SUMMARY]`）；
  本地 70 次运行没有一次触发，**没有找到根因**，不要读成"问题已解决"。记录见 `docs/evidence/pr26-retry-root-cause-20261105.md`。
- 其余集成夹具仍把 `utc()` 当作车端服务的单调时钟；如果它们出现偶发，先用 `steady_clock` 做干预验证。

**未验证或未执行（不得算作通过）**
- 真实 WebRTC 双端会话（浏览器或 Windows 查看器对 Orin）、5G / TURN、真相机与编码硬件、真实 CAN 与车辆、现场验收。
- Orin 上没有跑通的 8 项：缺 `avdec_h264` 的 6 项（3 跳过、3 失败）、缺 turnserver 的 1 项（跳过）、内核没有 netem 的 1 项（跳过）。
  它们在容器里通过（真实 netem 丢包用例只在容器里执行过）。
- `docker buildx` 发布路径只做了 `bash -n` 语法检查，没有端到端构建。
- Windows 与 Orin 的 30 分钟 mock 控制验收仍未通过。
- 瞬态保持没有在真实网络尖峰下验收；只有真实回环上的夹具和容器内的 netem 丢包。
- 保持对中转链路（云端 WSS 走 TCP）的收益取决于重传退避，需要现场用保持/升级统计验证（见 docs/DEPLOY.md）。
